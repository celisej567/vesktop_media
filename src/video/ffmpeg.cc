#include "video/ffmpeg.h"

#include <api/video/i420_buffer.h>
#include <api/video/video_frame.h>
#include <modules/video_coding/include/video_error_codes.h>

#include <cstring>
#include <functional>
#include <vector>

#include "common/log.h"
#include "common/platform.h"

namespace vv {

namespace {

// Decoders are looked up by name: codec ids are not stable across FFmpeg versions (173 is HEVC in
// FFmpeg 4, "fic" in FFmpeg 9)
constexpr const char* kH264 = "h264";
constexpr const char* kHevc = "hevc";
constexpr int kPixFmtYuv420p = 0;
constexpr int kPixFmtYuvj420p = 12;
constexpr int kAverrorEagain = -11;

// Leading fields, unchanged since FFmpeg 4
struct AvPacketHead {
    void* buf;
    int64_t pts, dts;
    uint8_t* data;
    int size;
};
struct AvFrameHead {
    uint8_t* data[8];
    int linesize[8];
    uint8_t** extendedData;
    int width, height;
    int nbSamples;
    int format;
};

struct Api {
    std::string source;
    unsigned (*avcodec_version)() = nullptr;
    const void* (*avcodec_find_decoder_by_name)(const char*) = nullptr;
    void* (*avcodec_alloc_context3)(const void*) = nullptr;
    int (*avcodec_open2)(void*, const void*, void**) = nullptr;
    void (*avcodec_free_context)(void**) = nullptr;
    int (*avcodec_send_packet)(void*, const void*) = nullptr;
    int (*avcodec_receive_frame)(void*, void*) = nullptr;
    void* (*av_packet_alloc)() = nullptr;
    void (*av_packet_free)(void**) = nullptr;
    void* (*av_frame_alloc)() = nullptr;
    void (*av_frame_free)(void**) = nullptr;
    void (*av_frame_unref)(void*) = nullptr;
    int (*av_opt_set)(void*, const char*, const char*, int) = nullptr;

    using Resolver = std::function<void*(const char*)>;
    bool Load(const Resolver& handle, const Resolver& utilHandle)
    {
#define VV_SYM(name, h) name = reinterpret_cast<decltype(name)>(h(#name))
        VV_SYM(avcodec_version, handle);
        VV_SYM(avcodec_find_decoder_by_name, handle);
        VV_SYM(avcodec_alloc_context3, handle);
        VV_SYM(avcodec_open2, handle);
        VV_SYM(avcodec_free_context, handle);
        VV_SYM(avcodec_send_packet, handle);
        VV_SYM(avcodec_receive_frame, handle);
        VV_SYM(av_packet_alloc, handle);
        VV_SYM(av_packet_free, handle);
        VV_SYM(av_frame_alloc, utilHandle);
        VV_SYM(av_frame_free, utilHandle);
        VV_SYM(av_frame_unref, utilHandle);
        VV_SYM(av_opt_set, utilHandle);
#undef VV_SYM
        return avcodec_version && avcodec_find_decoder_by_name && avcodec_alloc_context3 && avcodec_open2 && avcodec_free_context &&
               avcodec_send_packet && avcodec_receive_frame && av_packet_alloc && av_packet_free && av_frame_alloc &&
               av_frame_free && av_frame_unref;
    }
    bool Has(const char* decoder) const { return avcodec_find_decoder_by_name && avcodec_find_decoder_by_name(decoder) != nullptr; }
};

// The host process first (Electron's libffmpeg), then system FFmpeg versions
const Api* ApiFor(const char* decoder)
{
    static std::vector<Api> apis = [] {
        std::vector<Api> list;
        auto log = [](const Api& a) {
            VV_LOGI("video", "ffmpeg %s (avcodec %u.%u): h264=%d h265=%d", a.source.c_str(), a.avcodec_version() >> 16,
                    (a.avcodec_version() >> 8) & 0xff, a.Has(kH264), a.Has(kHevc));
        };
        auto in = [](void* lib) { return [lib](const char* name) { return LibrarySymbol(lib, name); }; };
        // ours first (scripts/build-ffmpeg.sh, next to the module): H.264 and H.265 without the system's FFmpeg.
        // One library with -Bsymbolic, loaded without isolation (malloc / free stay the process')
        if (void* h = OpenLibrary({"libvesktop_ffmpeg.so"}, false)) {
            Api a;
            a.source = "bundled libvesktop_ffmpeg";
            if (a.Load(in(h), in(h))) {
                log(a);
                list.push_back(a);
            }
        }
        Api host;
        host.source = "host libffmpeg";
        if (host.Load(HostSymbol, HostSymbol)) {
            log(host);
            list.push_back(host);
        }
        auto covered = [&](const char* decoder) {
            for (auto& a : list)
                if (a.Has(decoder)) return true;
            return false;
        };
        // A system FFmpeg only for what the host lacks (Electron's libffmpeg has H.264 but no HEVC):
        // the newest one that loads, FFmpeg 9 … 4.4 (the struct prefixes above are the same in all)
        for (int v : {63, 62, 61, 60, 59, 58}) {
            if (covered(kH264) && covered(kHevc)) break;
            auto codec = "libavcodec.so." + std::to_string(v);
            void* h = OpenLibrary({codec.c_str()}, true);
            if (!h) continue;
            // libavutil is a dependency of libavcodec and already loaded with it (avutil major = avcodec major - 2)
            void* util = LoadedLibrary(("libavutil.so." + std::to_string(v - 2)).c_str());
            Api a;
            a.source = codec;
            if (util && a.Load(in(h), in(util))) {
                log(a);
                list.push_back(a);
                break;
            }
            CloseLibrary(h);
        }
        if (!covered(kHevc)) VV_LOGI("video", "no FFmpeg with an HEVC decoder: H.265 is not offered");
        if (!covered(kH264)) VV_LOGW("video", "no FFmpeg with an H.264 decoder: H.264 is not offered");
        return list;
    }();
    for (auto& a : apis)
        if (a.Has(decoder)) return &a;
    return nullptr;
}

const char* DecoderName(FfmpegCodec codec)
{
    return codec == FfmpegCodec::H264 ? kH264 : kHevc;
}

class FfmpegDecoder : public webrtc::VideoDecoder {
public:
    FfmpegDecoder(const Api* api, FfmpegCodec codec) : api_(api), codec_(codec) {}
    ~FfmpegDecoder() override { Release(); }

    bool Configure(const Settings&) override
    {
        Release();
        const void* decoder = api_->avcodec_find_decoder_by_name(DecoderName(codec_));
        ctx_ = api_->avcodec_alloc_context3(decoder);
        if (!ctx_) return false;
        if (api_->av_opt_set) {
            api_->av_opt_set(ctx_, "threads", "auto", 1);
            api_->av_opt_set(ctx_, "thread_type", "slice", 1); // frame threading adds latency
            api_->av_opt_set(ctx_, "flags", "+low_delay", 1);
        }
        if (api_->avcodec_open2(ctx_, decoder, nullptr) < 0) {
            api_->avcodec_free_context(&ctx_);
            return false;
        }
        packet_ = api_->av_packet_alloc();
        frame_ = api_->av_frame_alloc();
        return packet_ && frame_;
    }

    int32_t Decode(const webrtc::EncodedImage& input, int64_t) override
    {
        if (!ctx_ || !callback_) return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
        if (!input.data() || input.size() == 0) return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
        // FFmpeg wants zeroed padding after the data
        buffer_.assign(input.data(), input.data() + input.size());
        buffer_.resize(input.size() + 64, 0);
        auto* pkt = static_cast<AvPacketHead*>(packet_);
        pkt->data = buffer_.data();
        pkt->size = static_cast<int>(input.size());
        int rc = api_->avcodec_send_packet(ctx_, packet_);
        pkt->data = nullptr;
        pkt->size = 0;
        if (rc < 0 && rc != kAverrorEagain) {
            if (errors_++ < 5) VV_LOGW("video", "ffmpeg: decoding a %zu byte frame failed (%d)", input.size(), rc);
            return WEBRTC_VIDEO_CODEC_ERROR;
        }

        bool got = false;
        while (api_->avcodec_receive_frame(ctx_, frame_) == 0) {
            auto* f = static_cast<AvFrameHead*>(frame_);
            if (f->format == kPixFmtYuv420p || f->format == kPixFmtYuvj420p) {
                auto i420 = webrtc::I420Buffer::Copy(f->width, f->height, f->data[0], f->linesize[0], f->data[1],
                                                     f->linesize[1], f->data[2], f->linesize[2]);
                auto frame = webrtc::VideoFrame::Builder()
                                 .set_video_frame_buffer(i420)
                                 .set_rtp_timestamp(input.RtpTimestamp())
                                 .set_color_space(input.ColorSpace() ? std::optional(*input.ColorSpace()) : std::nullopt)
                                 .build();
                callback_->Decoded(frame, std::nullopt, std::nullopt);
                got = true;
            } else if (!warnedFormat_) {
                warnedFormat_ = true;
                VV_LOGW("video", "unsupported decoded pixel format %d", f->format);
            }
            api_->av_frame_unref(frame_);
        }
        return got || rc == 0 ? WEBRTC_VIDEO_CODEC_OK : WEBRTC_VIDEO_CODEC_ERROR;
    }

    int32_t RegisterDecodeCompleteCallback(webrtc::DecodedImageCallback* callback) override
    {
        callback_ = callback;
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t Release() override
    {
        if (ctx_) api_->avcodec_free_context(&ctx_);
        if (packet_) api_->av_packet_free(&packet_);
        if (frame_) api_->av_frame_free(&frame_);
        return WEBRTC_VIDEO_CODEC_OK;
    }

    DecoderInfo GetDecoderInfo() const override
    {
        DecoderInfo info;
        info.implementation_name = "FFmpeg";
        info.is_hardware_accelerated = false;
        return info;
    }

private:
    const Api* api_;
    FfmpegCodec codec_;
    void* ctx_ = nullptr;
    void* packet_ = nullptr;
    void* frame_ = nullptr;
    std::vector<uint8_t> buffer_;
    webrtc::DecodedImageCallback* callback_ = nullptr;
    bool warnedFormat_ = false;
    int errors_ = 0;
};

} // namespace

bool FfmpegCanDecode(FfmpegCodec codec)
{
    return ApiFor(DecoderName(codec)) != nullptr;
}

std::unique_ptr<webrtc::VideoDecoder> CreateFfmpegDecoder(FfmpegCodec codec)
{
    auto* api = ApiFor(DecoderName(codec));
    if (!api) return nullptr;
    return std::make_unique<FfmpegDecoder>(api, codec);
}

} // namespace vv
