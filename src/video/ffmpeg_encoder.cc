#include "video/ffmpeg_encoder.h"

#include <api/video/encoded_image.h>
#include <api/video/i420_buffer.h>
#include <api/video/video_frame.h>
#include <modules/video_coding/codecs/h264/include/h264_globals.h>
#include <modules/video_coding/include/video_codec_interface.h>
#include <modules/video_coding/include/video_error_codes.h>
#include <rtc_base/time_utils.h>

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
constexpr int kAverrorEof = -541478725; // FFERRTAG('E','O','F',' ')
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
    // our FFmpeg only (scripts/vesktop_hw.c): VA-API needs frames in GPU surfaces, set up with FFmpeg's headers
    int (*av_vesktop_vaapi_open)(void*, int, int, const char*, char*, int) = nullptr;
    void (*av_log_set_callback)(void (*)(void*, int, const char*, va_list)) = nullptr;
    int (*av_log_format_line2)(void*, int, const char*, va_list, char*, int, int*) = nullptr;
    int (*av_strerror)(int, char*, size_t) = nullptr;
    int (*av_vesktop_vaapi_send)(void*, const void*) = nullptr;

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
        VV_SYM(av_vesktop_vaapi_open, codec);
        VV_SYM(av_vesktop_vaapi_send, codec);
        VV_SYM(av_log_set_callback, util);
        VV_SYM(av_log_format_line2, util);
        VV_SYM(av_strerror, util);
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

// FFmpeg's own messages (and through it libva's, AMF's, x264's, x265's) from warnings up into our log, as
// "ffmpeg: [h264_vaapi @ 0x…] …": the reason an encoder does not open or fails is there. A line repeated right away
// is counted, not written again; at most 500 lines a process (a failure on every frame would fill the log)
std::atomic<const Api*> gFfmpegLogApi{nullptr}; // whose av_log_format_line2 formats the lines
std::mutex gFfmpegLogMutex;
std::string gFfmpegLastLine;
int gFfmpegRepeats = 0, gFfmpegLines = 0;

void FfmpegLog(void* ptr, int level, const char* fmt, va_list args)
{
    if (level > 24) return; // AV_LOG_WARNING
    const Api* api = gFfmpegLogApi;
    char line[1024];
    thread_local int printPrefix = 1;
    if (api && api->av_log_format_line2) api->av_log_format_line2(ptr, level, fmt, args, line, sizeof(line), &printPrefix);
    else vsnprintf(line, sizeof(line), fmt, args);
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    if (!n) return;
    std::lock_guard lock(gFfmpegLogMutex);
    if (line == gFfmpegLastLine) {
        gFfmpegRepeats++;
        return;
    }
    if (gFfmpegRepeats) VV_LOGW("ffmpeg", "(the line before repeated %d times)", gFfmpegRepeats);
    gFfmpegRepeats = 0;
    gFfmpegLastLine = line;
    if (gFfmpegLines++ < 500) {
        if (level <= 16) VV_LOGE("ffmpeg", "%s", line);
        else VV_LOGW("ffmpeg", "%s", line);
    }
}

void SetUpLogging(Api* a)
{
    if (a->av_log_set_level) a->av_log_set_level(24); // AV_LOG_WARNING
    if (a->av_log_set_callback) {
        gFfmpegLogApi = a;
        a->av_log_set_callback(FfmpegLog);
    }
}

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
                SetUpLogging(a);
                return a;
            }
            delete a;
        }
        for (int v : {63, 62, 61, 60, 59, 58}) {
#ifdef _WIN32
            // the FFmpeg DLLs shipped next to the module (scripts/build-ffmpeg.ps1), never one from PATH
            auto codec = "avcodec-" + std::to_string(v) + ".dll";
            std::string error;
            void* h = OpenLocalLibrary(codec.c_str(), nullptr, &error);
            // there but not loadable
            if (!h && error.rfind("cannot be loaded", 0) == 0) VV_LOGW("video", "%s %s", codec.c_str(), error.c_str());
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
                SetUpLogging(a);
                return a;
            }
            delete a;
            CloseLibrary(h);
        }
        VV_LOGI("video", "no FFmpeg: H.264 / H.265 are not encoded");
        return nullptr;
    }();
    return api;
}

enum class Kind { Nvenc, Amf, Vaapi, Software };

struct Backend {
    const char* encoder;
    bool hardware;
    Kind kind;
    // FFmpeg sets the bitrate when it opens the encoder only: a new one (and a key frame) for a new bitrate
    bool reopenForBitrate;
};

// In order of preference: NVIDIA (NVENC), AMD on Windows (AMF, the BtbN build has it), AMD and Intel on Linux
// (VA-API, our build), the CPU. An encoder this FFmpeg lacks is not found and skipped
const std::vector<Backend>& Backends(EncodeCodec codec)
{
    static const std::vector<Backend> h264 = {{"h264_nvenc", true, Kind::Nvenc, false},
                                              {"h264_amf", true, Kind::Amf, true},
                                              {"h264_vaapi", true, Kind::Vaapi, true},
                                              {"libx264", false, Kind::Software, false}};
    static const std::vector<Backend> h265 = {{"hevc_nvenc", true, Kind::Nvenc, false},
                                              {"hevc_amf", true, Kind::Amf, true},
                                              {"hevc_vaapi", true, Kind::Vaapi, true},
                                              {"libx265", false, Kind::Software, true}};
    return codec == EncodeCodec::H264 ? h264 : h265;
}

const char* FamilyName(Kind kind)
{
    switch (kind) {
    case Kind::Nvenc: return "nvenc";
    case Kind::Amf: return "amf";
    case Kind::Vaapi: return "vaapi";
    case Kind::Software: return "software";
    }
    return "software";
}

// The DRM render node VA-API encoding worked on (VESKTOP_MEDIA_VAAPI_DEVICE picks one), found on the first open
std::mutex gVaapiDeviceMutex;
std::string gVaapiDevice;

std::vector<std::string> VaapiDevices()
{
    if (const char* env = getenv("VESKTOP_MEDIA_VAAPI_DEVICE"); env && *env) return {env};
    {
        std::lock_guard lock(gVaapiDeviceMutex);
        if (!gVaapiDevice.empty()) return {gVaapiDevice};
    }
    std::vector<std::string> devices;
    for (int i = 128; i < 140; i++) {
        std::string path = "/dev/dri/renderD" + std::to_string(i);
        if (FILE* f = fopen(path.c_str(), "r")) {
            fclose(f);
            devices.push_back(path);
        }
    }
    return devices;
}

std::mutex gPreferenceMutex;
// VESKTOP_MEDIA_ENCODER=hardware | software | an encoder's name, e.g. h264_vaapi (tests, troubleshooting)
std::string gPreference = [] {
    const char* env = getenv("VESKTOP_MEDIA_ENCODER");
    return std::string(env && *env ? env : "auto");
}();

struct Params {
    int width, height, fps;
    int bitrate; // bps
    int keyframeInterval;
};

// Encoder variants for comparisons (scripts and the encoder benchmark), unset in normal use:
// VESKTOP_MEDIA_NVENC_PRESET (p1 … p7, p6 by default), VESKTOP_MEDIA_NVENC_MULTIPASS (disabled, qres, fullres by default),
// VESKTOP_MEDIA_ENCODER_VBV (rate control buffer in seconds, 0.5 by default),
// VESKTOP_MEDIA_ENCODER_UNTRUSTED=1 (libwebrtc's frame dropper also for hardware encoders)
const char* EnvOr(const char* name, const char* fallback)
{
    const char* v = getenv(name);
    return v && *v ? v : fallback;
}

int BufferSize(int bitrate)
{
    static const double seconds = atof(EnvOr("VESKTOP_MEDIA_ENCODER_VBV", "0.5"));
    return std::max(static_cast<int>(bitrate * (seconds > 0 ? seconds : 0.5)), 100000);
}

std::string ErrorText(const Api* api, int rc)
{
    char text[128] = {};
    if (!api->av_strerror || api->av_strerror(rc, text, sizeof(text)) < 0) snprintf(text, sizeof(text), "error %d", rc);
    return text;
}

void* OpenContextOn(const Api* api, const Backend& b, EncodeCodec codec, const Params& p, const char* device);

// Opens an encoder context with our low latency settings; null when it doesn't open (no GPU, driver, …). VA-API:
// on the first render node it opens on
void* OpenContext(const Api* api, const Backend& b, EncodeCodec codec, const Params& p)
{
    if (b.kind != Kind::Vaapi) return OpenContextOn(api, b, codec, p, nullptr);
    if (!api->av_vesktop_vaapi_open || !api->av_vesktop_vaapi_send) return nullptr; // a system FFmpeg
    for (auto& device : VaapiDevices()) {
        if (void* ctx = OpenContextOn(api, b, codec, p, device.c_str())) {
            std::lock_guard lock(gVaapiDeviceMutex);
            if (gVaapiDevice != device) VV_LOGI("video", "%s on %s", b.encoder, device.c_str());
            gVaapiDevice = device;
            return ctx;
        }
    }
    return nullptr;
}

void* OpenContextOn(const Api* api, const Backend& b, EncodeCodec codec, const Params& p, const char* device)
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
    // In frames: NVENC and x265 take their frame rate, and with it each frame's share of the bitrate, from the time
    // base when it is not given otherwise (AVCodecContext::framerate has no option). A 1/90000 time base (the RTP
    // clock) made them budget for 90000 frames a second: a fraction of the bitrate and a blurred picture on motion
    snprintf(tb, sizeof(tb), "1/%d", std::max(1, p.fps));
    set("time_base", tb);
    seti("b", p.bitrate);
    seti("maxrate", p.bitrate);
    seti("bufsize", BufferSize(p.bitrate)); // half a second: few spikes, quick to react
    seti("g", p.keyframeInterval);
    seti("bf", 0); // B-frames add a frame of delay each
    set("forced-idr", "1");
    if (b.kind == Kind::Nvenc) {
        // p6 with a full resolution first pass, as Discord's own encoder for H.265: less blur on motion at the same
        // bitrate (docs/05-video-send.md). Newer presets p1 … p7 with tune ll; FFmpeg < 4.3 knows only the old names
        if (!set("preset", EnvOr("VESKTOP_MEDIA_NVENC_PRESET", "p6")) || !set("tune", "ll")) set("preset", "llhq");
        set("multipass", EnvOr("VESKTOP_MEDIA_NVENC_MULTIPASS", "fullres"));
        set("rc", "cbr");
        set("zerolatency", "1");
        set("delay", "0");
        if (codec == EncodeCodec::H264) set("profile", "high");
    } else if (b.kind == Kind::Amf) {
        // low latency usage, the best quality preset, constant bitrate; a requested key frame is an IDR with its
        // parameter sets (forced_idr), and they also come before every periodic one
        set("usage", "lowlatency");
        set("quality", "quality");
        set("rc", "cbr");
        set("latency", "1");
        set("forced_idr", "1");
        if (codec == EncodeCodec::H264) {
            set("profile", "high");
            seti("header_spacing", p.keyframeInterval);
        } else {
            set("header_insertion_mode", "idr");
        }
    } else if (b.kind == Kind::Vaapi) {
        // constant bitrate, one frame in flight; the parameter sets come with every IDR (FFmpeg's VA-API encoders
        // write them as packed headers), a requested key frame (pict_type I) becomes an IDR
        set("rc_mode", "CBR");
        set("async_depth", "1");
        if (codec == EncodeCodec::H264) set("profile", "high");
        char vendor[256] = {};
        if (int rc = api->av_vesktop_vaapi_open(ctx, p.width, p.height, device, vendor, sizeof(vendor)); rc < 0) {
            VV_LOGI("video", "%s: no VA-API device on %s (%s)", b.encoder, device ? device : "default", ErrorText(api, rc).c_str());
            api->avcodec_free_context(&ctx);
            return nullptr;
        }
        VV_LOGI("video", "%s: VA-API driver on %s: %s", b.encoder, device ? device : "default", vendor);
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
    if (int rc = api->avcodec_open2(ctx, enc, nullptr); rc < 0) {
        // FFmpeg's own message, the reason, is in the log right before (ffmpeg: …)
        VV_LOGI("video", "%s: does not open at %dx%d (%s)", b.encoder, p.width, p.height, ErrorText(api, rc).c_str());
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
    // a named encoder (VESKTOP_MEDIA_ENCODER) or a family (the setting) first
    for (auto& b : Backends(codec))
        if ((pref == b.encoder || (b.kind != Kind::Software && pref == FamilyName(b.kind))) && Works(api, b, codec)) return &b;
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
        CloseContext();
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
        // pts: the RTP timestamp (90 kHz), unwrapped, in frames of the time base; increasing, so frames that come
        // faster than the frame rate still get their own
        uint32_t ts = frame.rtp_timestamp();
        if (havePts_) rtpUnwrapped_ += static_cast<uint32_t>(ts - lastRtp_);
        lastRtp_ = ts;
        int64_t pts = rtpUnwrapped_ * openFps_ / 90000;
        if (havePts_ && pts <= lastPts_) pts = lastPts_ + 1;
        havePts_ = true;
        lastPts_ = pts;
        *reinterpret_cast<int64_t*>(reinterpret_cast<uint8_t*>(frame_) + kFramePtsOffset) = pts;
        *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(frame_) + api_->pictTypeOffset) = key ? kPictureTypeI : 0;

        int rc = backend_->kind == Kind::Vaapi ? api_->av_vesktop_vaapi_send(ctx_, frame_) : api_->avcodec_send_frame(ctx_, frame_);
        if (rc < 0 && rc != kAverrorEagain) {
            if (errors_++ < 5) VV_LOGW("video", "%s: encoding failed (%s)", backend_->encoder, ErrorText(api_, rc).c_str());
            return WEBRTC_VIDEO_CODEC_ERROR;
        }
        while ((rc = api_->avcodec_receive_packet(ctx_, packet_)) == 0) {
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
        // EAGAIN: wants the next frame; anything else is a failure of the encoder (a driver error)
        if (rc != kAverrorEagain && rc != kAverrorEof && errors_++ < 5)
            VV_LOGW("video", "%s: no packet (%s)", backend_->encoder, ErrorText(api_, rc).c_str());
        return WEBRTC_VIDEO_CODEC_OK;
    }

    void SetRates(const RateControlParameters& rates) override
    {
        int bps = static_cast<int>(rates.bitrate.get_sum_bps());
        if (bps <= 0) return;
        if (backend_->reopenForBitrate) {
            // every change is a new encoder and a key frame: follow a drop of more than 15 % at once (the pacer
            // would hold back what the encoder makes beyond the estimate), a rise of more than 25 % at most every 3 s
            int64_t now = webrtc::TimeMillis();
            if (bps < params_.bitrate * 85 / 100 || (bps > params_.bitrate * 125 / 100 && now - lastReopenMs_ >= 3000))
                pendingBitrate_ = bps;
        } else if (std::abs(bps - params_.bitrate) * 10 > params_.bitrate) {
            pendingBitrate_ = bps; // only noticeable changes
        }
        if (rates.framerate_fps > 1) params_.fps = static_cast<int>(rates.framerate_fps + 0.5);
    }

    EncoderInfo GetEncoderInfo() const override
    {
        EncoderInfo info;
        info.implementation_name = std::string("FFmpeg ") + backend_->encoder;
        info.is_hardware_accelerated = backend_->hardware;
        info.supports_native_handle = false;
        static const bool untrusted = getenv("VESKTOP_MEDIA_ENCODER_UNTRUSTED") != nullptr;
        info.has_trusted_rate_controller = backend_->hardware && !untrusted;
        info.requested_resolution_alignment = 2;
        info.scaling_settings = webrtc::VideoEncoder::ScalingSettings::kOff; // no QP reported
        return info;
    }

private:
    // Ends the encoder's stream and takes what it still holds before freeing it: a hardware encoder's driver threads
    // finish with the session first (one crash was in NVIDIA's libnvcuvid, a few seconds after a session was freed
    // while another one ran)
    void CloseContext()
    {
        if (!ctx_) return;
        int rc = backend_->kind == Kind::Vaapi ? api_->av_vesktop_vaapi_send(ctx_, nullptr) : api_->avcodec_send_frame(ctx_, nullptr);
        if (rc >= 0 && packet_)
            for (int i = 0; i < 64 && api_->avcodec_receive_packet(ctx_, packet_) == 0; i++) api_->av_packet_unref(packet_);
        api_->avcodec_free_context(&ctx_);
    }

    bool Open()
    {
        CloseContext();
        ctx_ = OpenContext(api_, *backend_, codec_, params_);
        if (!ctx_) {
            VV_LOGE("video", "%s: cannot open for %dx%d", backend_->encoder, params_.width, params_.height);
            return false;
        }
        if (!frame_) frame_ = api_->av_frame_alloc();
        if (!packet_) packet_ = api_->av_packet_alloc();
        openFps_ = std::max(1, params_.fps);
        havePts_ = false; // a new encoder starts its timestamps again
        rtpUnwrapped_ = 0;
        VV_LOGI("video", "%s: %dx%d @ %d, %d kbit/s", backend_->encoder, params_.width, params_.height, params_.fps,
                params_.bitrate / 1000);
        return frame_ && packet_;
    }

    void ApplyBitrate()
    {
        params_.bitrate = pendingBitrate_;
        pendingBitrate_ = 0;
        if (backend_->reopenForBitrate) {
            // no reconfiguration in FFmpeg's libx265, AMF and VA-API encoders: a new encoder (and a key frame)
            lastReopenMs_ = webrtc::TimeMillis();
            Open();
            return;
        }
        // libx264 and nvenc pick the new rate up on the next frame (x264_encoder_reconfig / NVENC reconfigure)
        api_->av_opt_set_int(ctx_, "b", params_.bitrate, 0);
        api_->av_opt_set_int(ctx_, "maxrate", params_.bitrate, 0);
        api_->av_opt_set_int(ctx_, "bufsize", BufferSize(params_.bitrate), 0);
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
    int64_t rtpUnwrapped_ = 0;
    int64_t lastPts_ = 0;
    int openFps_ = 30; // the time base of the open encoder: 1/openFps_
    int64_t lastReopenMs_ = 0;
    int errors_ = 0;
};

} // namespace

void SetEncoderPreference(const std::string& preference)
{
    std::lock_guard lock(gPreferenceMutex);
    static const char* kKnown[] = {"auto", "hardware", "software", "nvenc", "amf", "vaapi"};
    gPreference = "auto";
    for (const char* k : kKnown)
        if (preference == k) gPreference = k;
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

std::vector<WorkingEncoder> FfmpegWorkingEncoders(EncodeCodec codec)
{
    std::vector<WorkingEncoder> list;
    const Api* api = LoadApi();
    if (!api) return list;
    for (auto& b : Backends(codec))
        if (Works(api, b, codec)) list.push_back({b.encoder, FamilyName(b.kind)});
    return list;
}

std::unique_ptr<webrtc::VideoEncoder> CreateFfmpegEncoder(EncodeCodec codec)
{
    auto* b = PickBackend(codec);
    if (!b) return nullptr;
    return std::make_unique<FfmpegEncoder>(LoadApi(), b, codec);
}

} // namespace vv
