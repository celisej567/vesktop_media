#include "video/ffmpeg_encoder.h"

#include <api/video/encoded_image.h>
#include <api/video/i420_buffer.h>
#include <api/video/video_frame.h>
#include <modules/video_coding/codecs/h264/include/h264_globals.h>
#include <modules/video_coding/include/video_codec_interface.h>
#include <modules/video_coding/include/video_error_codes.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <map>
#include <cstring>
#include <mutex>
#include <vector>

#include "common/log.h"
#include "common/platform.h"

namespace vv {

namespace {

constexpr int kPixFmtYuv420p = 0;
constexpr int kAverrorEagain = -11;
constexpr int kPictureTypeI = 1;
constexpr int kPacketFlagKey = 1;

// Leading fields of AVPacket / AVFrame, unchanged since FFmpeg 4 (checked against the headers): AVFrame has
// pict_type right after `format` from libavutil 60 on (key_frame was removed), after key_frame before; pts is
// at 136 in both layouts
struct AvPacketHead {
    void* buf;
    int64_t pts, dts;
    uint8_t* data;
    int size;
    int streamIndex;
    int flags;
};
struct AvFrameHead {
    uint8_t* data[8];
    int linesize[8];
    uint8_t** extendedData;
    int width, height;
    int nbSamples;
    int format;
};
constexpr size_t kFramePtsOffset = 136;

constexpr const char* kBundledFfmpeg = VV_LOCAL_LIBRARY("ffmpeg");

struct Api {
    std::string source;
    unsigned (*avutil_version)() = nullptr;
    const void* (*avcodec_find_encoder_by_name)(const char*) = nullptr;
    void* (*avcodec_alloc_context3)(const void*) = nullptr;
    int (*avcodec_open2)(void*, const void*, void**) = nullptr;
    void (*avcodec_free_context)(void**) = nullptr;
    int (*avcodec_send_frame)(void*, const void*) = nullptr;
    int (*avcodec_receive_packet)(void*, void*) = nullptr;
    void* (*av_packet_alloc)() = nullptr;
    void (*av_packet_free)(void**) = nullptr;
    void (*av_packet_unref)(void*) = nullptr;
    void* (*av_frame_alloc)() = nullptr;
    void (*av_frame_free)(void**) = nullptr;
    int (*av_opt_set)(void*, const char*, const char*, int) = nullptr;
    int (*av_opt_set_int)(void*, const char*, int64_t, int) = nullptr;
    void (*av_log_set_level)(int) = nullptr;

    size_t pictTypeOffset = 120;

    bool Load(void* codec, void* util)
    {
#define VV_SYM(name, lib) name = reinterpret_cast<decltype(name)>(LibrarySymbol(lib, #name))
        VV_SYM(avcodec_find_encoder_by_name, codec);
        VV_SYM(avcodec_alloc_context3, codec);
        VV_SYM(avcodec_open2, codec);
        VV_SYM(avcodec_free_context, codec);
        VV_SYM(avcodec_send_frame, codec);
        VV_SYM(avcodec_receive_packet, codec);
        VV_SYM(av_packet_alloc, codec);
        VV_SYM(av_packet_free, codec);
        VV_SYM(av_packet_unref, codec);
        VV_SYM(avutil_version, util);
        VV_SYM(av_frame_alloc, util);
        VV_SYM(av_frame_free, util);
        VV_SYM(av_opt_set, util);
        VV_SYM(av_opt_set_int, util);
        VV_SYM(av_log_set_level, util);
#undef VV_SYM
        if (!(avutil_version && avcodec_find_encoder_by_name && avcodec_alloc_context3 && avcodec_open2 && avcodec_free_context &&
              avcodec_send_frame && avcodec_receive_packet && av_packet_alloc && av_packet_free && av_packet_unref &&
              av_frame_alloc && av_frame_free && av_opt_set && av_opt_set_int))
            return false;
        unsigned major = avutil_version() >> 16;
        if (major < 56 || major > 61) return false; // AVFrame / AVPacket prefixes not checked for other versions
        pictTypeOffset = major >= 60 ? 120 : 124;
        return true;
    }
};

// Ours (scripts/build-ffmpeg.sh, next to the module) first, not isolated; else the newest system FFmpeg (9 … 4.4)
// that loads, isolated (RTLD_DEEPBIND) from Electron's libffmpeg
const Api* LoadApi()
{
    static const Api* api = []() -> const Api* {
        // one library with -Bsymbolic (scripts/build-ffmpeg.sh): not isolated, so malloc / free stay the process'
        if (void* h = OpenLibrary({kBundledFfmpeg}, false)) {
            auto* a = new Api();
            a->source = std::string(kBundledFfmpeg) + " (bundled)";
            if (a->Load(h, h)) {
                if (a->av_log_set_level) a->av_log_set_level(16);
                return a;
            }
            delete a;
        }
        for (int v : {63, 62, 61, 60, 59, 58}) {
#ifdef _WIN32
            // the FFmpeg DLLs shipped next to the module (scripts/build-ffmpeg.ps1), never one from PATH
            auto codec = "avcodec-" + std::to_string(v) + ".dll";
            void* h = OpenLocalLibrary(codec.c_str());
            auto utilName = "avutil-" + std::to_string(v - 2) + ".dll";
#else
            auto codec = "libavcodec.so." + std::to_string(v);
            void* h = OpenLibrary({codec.c_str()}, true);
            auto utilName = "libavutil.so." + std::to_string(v - 2);
#endif
            if (!h) continue;
            void* util = LoadedLibrary(utilName.c_str());
            auto* a = new Api();
            a->source = codec;
            if (util && a->Load(h, util)) {
                if (a->av_log_set_level) a->av_log_set_level(16); // AV_LOG_ERROR: x264 / nvenc are chatty
                return a;
            }
            delete a;
            CloseLibrary(h);
        }
        VV_LOGI("video", "no system FFmpeg: H.264 / H.265 are not encoded");
        return nullptr;
    }();
    return api;
}

struct Backend {
    const char* encoder;
    bool hardware;
};

// In order of preference
const std::vector<Backend>& Backends(EncodeCodec codec)
{
    static const std::vector<Backend> h264 = {{"h264_nvenc", true}, {"libx264", false}};
    static const std::vector<Backend> h265 = {{"hevc_nvenc", true}, {"libx265", false}};
    return codec == EncodeCodec::H264 ? h264 : h265;
}

std::mutex gPreferenceMutex;
// VESKTOP_MEDIA_ENCODER=hardware | software (tests, troubleshooting)
std::string gPreference = [] {
    const char* env = getenv("VESKTOP_MEDIA_ENCODER");
    return std::string(env && (!strcmp(env, "hardware") || !strcmp(env, "software")) ? env : "auto");
}();

struct Params {
    int width, height, fps;
    int bitrate; // bps
    int keyframeInterval;
};

// Opens an encoder context with our low latency settings; null when it doesn't open (no GPU, driver, …)
void* OpenContext(const Api* api, const Backend& b, EncodeCodec codec, const Params& p)
{
    const void* enc = api->avcodec_find_encoder_by_name(b.encoder);
    if (!enc) return nullptr;
    void* ctx = api->avcodec_alloc_context3(enc);
    if (!ctx) return nullptr;
    auto set = [&](const char* k, const char* v) { return api->av_opt_set(ctx, k, v, 1 /* AV_OPT_SEARCH_CHILDREN */) >= 0; };
    auto seti = [&](const char* k, int64_t v) { return api->av_opt_set_int(ctx, k, v, 1) >= 0; };
    char size[32];
    snprintf(size, sizeof(size), "%dx%d", p.width, p.height);
    set("video_size", size);
    set("pixel_format", "yuv420p");
    char tb[32];
    snprintf(tb, sizeof(tb), "1/%d", 90000); // RTP clock: pts = the frame's RTP timestamp
    set("time_base", tb);
    seti("b", p.bitrate);
    seti("maxrate", p.bitrate);
    seti("bufsize", std::max(p.bitrate / 2, 100000)); // half a second: few spikes, quick to react
    seti("g", p.keyframeInterval);
    seti("bf", 0); // B-frames add a frame of delay each
    set("forced-idr", "1");
    bool nvenc = strstr(b.encoder, "nvenc") != nullptr;
    if (nvenc) {
        // newer presets p1 … p7 with tune ll; FFmpeg < 4.3 knows only the old names
        if (!set("preset", "p4") || !set("tune", "ll")) set("preset", "llhq");
        set("rc", "cbr");
        set("zerolatency", "1");
        set("delay", "0");
        if (codec == EncodeCodec::H264) set("profile", "high");
    } else if (codec == EncodeCodec::H264) {
        set("preset", "veryfast");
        set("tune", "zerolatency");
        set("profile", "high");
        set("threads", "0");
        // SPS / PPS before every key frame (a viewer joining needs them); Annex B is FFmpeg's default
        set("x264-params", "repeat-headers=1:nal-hrd=none");
    } else {
        set("preset", "superfast");
        set("tune", "zerolatency");
        set("threads", "0");
        set("x265-params", "repeat-headers=1:log-level=error");
    }
    if (api->avcodec_open2(ctx, enc, nullptr) < 0) {
        api->avcodec_free_context(&ctx);
        return nullptr;
    }
    return ctx;
}

// Whether a backend really works here: opened once on a small picture, the result cached
bool Works(const Api* api, const Backend& b, EncodeCodec codec)
{
    static std::mutex mutex;
    static std::map<std::string, bool> cache;
    std::lock_guard lock(mutex);
    auto it = cache.find(b.encoder);
    if (it != cache.end()) return it->second;
    void* ctx = OpenContext(api, b, codec, {256, 144, 30, 300000, 300});
    bool ok = ctx != nullptr;
    if (ctx) api->avcodec_free_context(&ctx);
    cache[b.encoder] = ok;
    VV_LOGI("video", "encoder %s (%s): %s", b.encoder, api->source.c_str(), ok ? "available" : "not available");
    return ok;
}

const Backend* PickBackend(EncodeCodec codec)
{
    const Api* api = LoadApi();
    if (!api) return nullptr;
    std::string pref = EncoderPreference();
    for (auto& b : Backends(codec)) {
        if (pref == "hardware" && !b.hardware) continue;
        if (pref == "software" && b.hardware) continue;
        if (Works(api, b, codec)) return &b;
    }
    // a preference that can't be met falls back to whatever works
    if (pref != "auto")
        for (auto& b : Backends(codec))
            if (Works(api, b, codec)) return &b;
    return nullptr;
}

class FfmpegEncoder : public webrtc::VideoEncoder {
public:
    FfmpegEncoder(const Api* api, const Backend* backend, EncodeCodec codec) : api_(api), backend_(backend), codec_(codec) {}
    ~FfmpegEncoder() override { Release(); }

    int InitEncode(const webrtc::VideoCodec* settings, const Settings&) override
    {
        if (!settings || settings->width < 2 || settings->height < 2) return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
        params_.width = settings->width & ~1;
        params_.height = settings->height & ~1;
        params_.fps = settings->maxFramerate > 0 ? static_cast<int>(settings->maxFramerate) : 30;
        params_.bitrate = std::max(100, static_cast<int>(settings->startBitrate)) * 1000;
        // a key frame on request (PLI) and every 60 s otherwise
        params_.keyframeInterval = params_.fps * 60;
        return Open() ? WEBRTC_VIDEO_CODEC_OK : WEBRTC_VIDEO_CODEC_ERROR;
    }

    int32_t RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback* callback) override
    {
        callback_ = callback;
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t Release() override
    {
        if (ctx_) api_->avcodec_free_context(&ctx_);
        if (frame_) api_->av_frame_free(&frame_);
        if (packet_) api_->av_packet_free(&packet_);
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t Encode(const webrtc::VideoFrame& frame, const std::vector<webrtc::VideoFrameType>* types) override
    {
        if (!ctx_ || !callback_) return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
        auto i420 = frame.video_frame_buffer()->ToI420();
        if (!i420) return WEBRTC_VIDEO_CODEC_ERROR;
        int w = i420->width() & ~1, h = i420->height() & ~1;
        if (w != params_.width || h != params_.height) {
            // the source changed size (adaptation, another window): a new stream with a key frame
            params_.width = w;
            params_.height = h;
            if (!Open()) return WEBRTC_VIDEO_CODEC_ERROR;
        }
        if (pendingBitrate_ > 0) ApplyBitrate();

        bool key = forceKey_.exchange(false);
        if (types)
            for (auto t : *types) key |= t == webrtc::VideoFrameType::kVideoFrameKey;

        auto* f = static_cast<AvFrameHead*>(frame_);
        f->data[0] = const_cast<uint8_t*>(i420->DataY());
        f->data[1] = const_cast<uint8_t*>(i420->DataU());
        f->data[2] = const_cast<uint8_t*>(i420->DataV());
        f->linesize[0] = i420->StrideY();
        f->linesize[1] = i420->StrideU();
        f->linesize[2] = i420->StrideV();
        f->width = w;
        f->height = h;
        f->format = kPixFmtYuv420p;
        // pts: the RTP timestamp, unwrapped (the time base is 1/90000)
        uint32_t ts = frame.rtp_timestamp();
        if (havePts_) ptsBase_ += static_cast<uint32_t>(ts - lastRtp_);
        havePts_ = true;
        lastRtp_ = ts;
        *reinterpret_cast<int64_t*>(reinterpret_cast<uint8_t*>(frame_) + kFramePtsOffset) = ptsBase_;
        *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(frame_) + api_->pictTypeOffset) = key ? kPictureTypeI : 0;

        int rc = api_->avcodec_send_frame(ctx_, frame_);
        if (rc < 0 && rc != kAverrorEagain) {
            if (errors_++ < 5) VV_LOGW("video", "%s: encoding failed (%d)", backend_->encoder, rc);
            return WEBRTC_VIDEO_CODEC_ERROR;
        }
        while (api_->avcodec_receive_packet(ctx_, packet_) == 0) {
            auto* p = static_cast<AvPacketHead*>(packet_);
            webrtc::EncodedImage image;
            image.SetEncodedData(webrtc::EncodedImageBuffer::Create(p->data, p->size));
            image.SetRtpTimestamp(frame.rtp_timestamp());
            image.capture_time_ms_ = frame.render_time_ms();
            image._encodedWidth = w;
            image._encodedHeight = h;
            image._frameType = (p->flags & kPacketFlagKey) ? webrtc::VideoFrameType::kVideoFrameKey : webrtc::VideoFrameType::kVideoFrameDelta;
            image.rotation_ = frame.rotation();
            webrtc::CodecSpecificInfo info;
            info.codecType = codec_ == EncodeCodec::H264 ? webrtc::kVideoCodecH264 : webrtc::kVideoCodecH265;
            if (codec_ == EncodeCodec::H264) {
                info.codecSpecific.H264.packetization_mode = webrtc::H264PacketizationMode::NonInterleaved;
                info.codecSpecific.H264.temporal_idx = webrtc::kNoTemporalIdx;
                info.codecSpecific.H264.idr_frame = image.IsKey();
                info.codecSpecific.H264.base_layer_sync = false;
            }
            callback_->OnEncodedImage(image, &info);
            api_->av_packet_unref(packet_);
        }
        return WEBRTC_VIDEO_CODEC_OK;
    }

    void SetRates(const RateControlParameters& rates) override
    {
        int bps = static_cast<int>(rates.bitrate.get_sum_bps());
        if (bps <= 0) return;
        // only noticeable changes: x265 has to be reopened for a new bitrate
        if (std::abs(bps - params_.bitrate) * 10 > params_.bitrate) pendingBitrate_ = bps;
        if (rates.framerate_fps > 1) params_.fps = static_cast<int>(rates.framerate_fps + 0.5);
    }

    EncoderInfo GetEncoderInfo() const override
    {
        EncoderInfo info;
        info.implementation_name = std::string("FFmpeg ") + backend_->encoder;
        info.is_hardware_accelerated = backend_->hardware;
        info.supports_native_handle = false;
        info.has_trusted_rate_controller = backend_->hardware;
        info.requested_resolution_alignment = 2;
        info.scaling_settings = webrtc::VideoEncoder::ScalingSettings::kOff; // no QP reported
        return info;
    }

private:
    bool Open()
    {
        if (ctx_) api_->avcodec_free_context(&ctx_);
        ctx_ = OpenContext(api_, *backend_, codec_, params_);
        if (!ctx_) {
            VV_LOGE("video", "%s: cannot open for %dx%d", backend_->encoder, params_.width, params_.height);
            return false;
        }
        if (!frame_) frame_ = api_->av_frame_alloc();
        if (!packet_) packet_ = api_->av_packet_alloc();
        VV_LOGI("video", "%s: %dx%d @ %d, %d kbit/s", backend_->encoder, params_.width, params_.height, params_.fps,
                params_.bitrate / 1000);
        return frame_ && packet_;
    }

    void ApplyBitrate()
    {
        params_.bitrate = pendingBitrate_;
        pendingBitrate_ = 0;
        if (strcmp(backend_->encoder, "libx265") == 0) {
            // no reconfiguration in FFmpeg's libx265: a new encoder (and a key frame)
            Open();
            return;
        }
        // libx264 and nvenc pick the new rate up on the next frame (x264_encoder_reconfig / NVENC reconfigure)
        api_->av_opt_set_int(ctx_, "b", params_.bitrate, 0);
        api_->av_opt_set_int(ctx_, "maxrate", params_.bitrate, 0);
        api_->av_opt_set_int(ctx_, "bufsize", std::max(params_.bitrate / 2, 100000), 0);
    }

    const Api* api_;
    const Backend* backend_;
    EncodeCodec codec_;
    Params params_{};
    void* ctx_ = nullptr;
    void* frame_ = nullptr;
    void* packet_ = nullptr;
    webrtc::EncodedImageCallback* callback_ = nullptr;
    std::atomic<bool> forceKey_{false};
    int pendingBitrate_ = 0;
    bool havePts_ = false;
    uint32_t lastRtp_ = 0;
    int64_t ptsBase_ = 0;
    int errors_ = 0;
};

} // namespace

void SetEncoderPreference(const std::string& preference)
{
    std::lock_guard lock(gPreferenceMutex);
    gPreference = preference == "hardware" || preference == "software" ? preference : "auto";
    VV_LOGI("video", "encoder preference: %s", gPreference.c_str());
}

std::string EncoderPreference()
{
    std::lock_guard lock(gPreferenceMutex);
    return gPreference;
}

bool FfmpegCanEncode(EncodeCodec codec)
{
    return PickBackend(codec) != nullptr;
}

std::string FfmpegEncoderName(EncodeCodec codec)
{
    auto* b = PickBackend(codec);
    return b ? b->encoder : "";
}

std::unique_ptr<webrtc::VideoEncoder> CreateFfmpegEncoder(EncodeCodec codec)
{
    auto* b = PickBackend(codec);
    if (!b) return nullptr;
    return std::make_unique<FfmpegEncoder>(LoadApi(), b, codec);
}

} // namespace vv
