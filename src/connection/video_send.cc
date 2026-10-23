// Video send: one VideoSendStream per connection,
// fed by the camera (default context) or the desktop capture (stream context), simulcast layers from
// streamParameters, VP8 (libvpx) or H.264 / H.265 (FFmpeg), DAVE frame encryption.
#include "connection/video_send.h"

#include <api/make_ref_counted.h>
#include <api/video/builtin_video_bitrate_allocator_factory.h>
#include <api/video_codecs/sdp_video_format.h>
#include <api/video_codecs/video_encoder.h>
#include <call/call.h>
#include <call/video_send_stream.h>
#include <media/engine/simulcast_encoder_adapter.h>
#include <modules/video_coding/codecs/vp8/include/vp8.h>
#include <rtc_base/thread.h>
#include <video/config/video_encoder_config.h>

#include <algorithm>
#include <cmath>
#include <random>

#include "capture/camera.h"
#include "capture/pipewire_capturer.h"
#include "capture/portal.h"
#include "audio/soundshare.h"
#include "common/log.h"
#include "connection/connection.h"
#include "video/ffmpeg_encoder.h"
#include "video/frame_source.h"

namespace vv {

namespace {

constexpr int kDefaultMaxBitrate = 2'500'000;
constexpr int kDefaultMinBitrate = 150'000;

std::string RandomCname()
{
    static const char chars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::random_device rd;
    std::string s;
    for (int i = 0; i < 16; i++) s += chars[rd() % (sizeof(chars) - 1)];
    return s;
}

// VP8 (libvpx), H.264 / H.265 (FFmpeg: NVENC or the CPU, video/ffmpeg_encoder.h)
class CodecFactory : public webrtc::VideoEncoderFactory {
public:
    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override
    {
        std::vector<webrtc::SdpVideoFormat> formats = {webrtc::SdpVideoFormat("VP8")};
        if (FfmpegCanEncode(EncodeCodec::H264)) formats.emplace_back("H264");
        if (FfmpegCanEncode(EncodeCodec::H265)) formats.emplace_back("H265");
        return formats;
    }
    std::unique_ptr<webrtc::VideoEncoder> Create(const webrtc::Environment& env, const webrtc::SdpVideoFormat& format) override
    {
        if (format.name == "VP8") return webrtc::CreateVp8Encoder(env);
        if (format.name == "H264") return CreateFfmpegEncoder(EncodeCodec::H264);
        if (format.name == "H265") return CreateFfmpegEncoder(EncodeCodec::H265);
        return nullptr;
    }
};

class EncoderFactory : public webrtc::VideoEncoderFactory {
public:
    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override { return inner_.GetSupportedFormats(); }
    std::unique_ptr<webrtc::VideoEncoder> Create(const webrtc::Environment& env, const webrtc::SdpVideoFormat& format) override
    {
        if (format.name != "VP8" && format.name != "H264" && format.name != "H265") return nullptr;
        return std::make_unique<webrtc::SimulcastEncoderAdapter>(env, &inner_, nullptr, format);
    }

private:
    CodecFactory inner_;
};

// One simulcast layer of streamParameters
struct Layer {
    std::string rid;
    uint32_t ssrc = 0, rtxSsrc = 0;
    int quality = 100;
    bool active = true;
    int maxBitrate = 0, minBitrate = 0, targetBitrate = 0;
    int maxFrameRate = 0, maxPixelCount = 0;
    bool sourceResolution = false;
    int maxWidth = 0, maxHeight = 0;
};

std::vector<Layer> VideoLayers(const json& params)
{
    std::vector<Layer> layers;
    if (!params.is_array()) return layers;
    for (auto& p : params) {
        if (Get<std::string>(p, "type", "") != "video") continue;
        Layer l;
        l.rid = Get<std::string>(p, "rid", "");
        l.ssrc = Get<uint32_t>(p, "ssrc", 0);
        l.rtxSsrc = Get<uint32_t>(p, "rtxSsrc", 0);
        if (!l.ssrc) continue;
        l.quality = Get(p, "quality", 100);
        l.active = Get(p, "active", true);
        l.maxBitrate = Get(p, "maxBitrate", 0);
        l.minBitrate = Get(p, "minBitrate", 0);
        l.targetBitrate = Get(p, "targetBitrate", 0);
        l.maxFrameRate = Get(p, "maxFrameRate", 0);
        l.maxPixelCount = Get(p, "maxPixelCount", 0);
        if (p.contains("maxResolution") && p["maxResolution"].is_object()) {
            auto& r = p["maxResolution"];
            l.sourceResolution = Get<std::string>(r, "type", "") == "source";
            l.maxWidth = Get(r, "width", 0);
            l.maxHeight = Get(r, "height", 0);
        }
        layers.push_back(l);
    }
    // libwebrtc wants simulcast streams from the lowest to the highest quality
    std::stable_sort(layers.begin(), layers.end(), [](const Layer& a, const Layer& b) { return a.quality < b.quality; });
    return layers;
}

int MinPositive(std::initializer_list<int> values, int fallback)
{
    int best = 0;
    for (int v : values)
        if (v > 0 && (best == 0 || v < best)) best = v;
    return best > 0 ? best : fallback;
}

struct LayerPlan {
    int width, height, fps;
    int minBitrate, targetBitrate, maxBitrate;
    bool active;
};

// Size, rate and bitrates of each layer for a `width` x `height` source
std::vector<LayerPlan> PlanLayers(const std::vector<Layer>& layers, const Connection::VideoSendOptions& o, int width, int height)
{
    std::vector<LayerPlan> plans;
    bool wanted = o.sinkWantsPixels > 0 || o.alwaysSend || o.screen;
    for (auto& l : layers) {
        double scale = 1.0;
        auto fit = [&](int boxW, int boxH) {
            if (boxW <= 0 || boxH <= 0) return;
            if ((width < height) != (boxW < boxH)) std::swap(boxW, boxH); // orientation agnostic
            scale = std::min({scale, static_cast<double>(boxW) / width, static_cast<double>(boxH) / height});
        };
        if (!l.sourceResolution) fit(l.maxWidth, l.maxHeight);
        fit(o.width, o.height);
        int pixels = MinPositive({l.maxPixelCount, o.sinkWantsPixels}, 0);
        if (pixels > 0) scale = std::min(scale, std::sqrt(static_cast<double>(pixels) / (static_cast<double>(width) * height)));
        if (l.maxWidth == 0 && !l.sourceResolution && l.quality < 100) scale = std::min(scale, l.quality / 100.0);
        LayerPlan p;
        p.width = std::max(16, static_cast<int>(width * scale) & ~1);
        p.height = std::max(16, static_cast<int>(height * scale) & ~1);
        p.fps = MinPositive({l.maxFrameRate, o.fps, o.sinkWantsFps}, 30);
        p.maxBitrate = MinPositive({l.maxBitrate, o.maxBitrate}, kDefaultMaxBitrate);
        p.minBitrate = std::min(p.maxBitrate, MinPositive({l.minBitrate, o.minBitrate}, kDefaultMinBitrate));
        p.targetBitrate = std::clamp(MinPositive({l.targetBitrate, o.bitrate}, p.maxBitrate), p.minBitrate, p.maxBitrate);
        if (o.bitrateOverride > 0) {
            // the user's bitrate for the full layer, a quarter for lower simulcast layers (as Discord's presets)
            p.maxBitrate = p.targetBitrate = l.quality < 100 ? o.bitrateOverride / 4 : o.bitrateOverride;
            p.minBitrate = std::min(p.minBitrate, p.maxBitrate);
        }
        p.active = l.active && wanted;
        plans.push_back(p);
    }
    return plans;
}

class LayerStreamFactory : public webrtc::VideoEncoderConfig::VideoStreamFactoryInterface {
public:
    LayerStreamFactory(std::vector<Layer> layers, Connection::VideoSendOptions options)
        : layers_(std::move(layers)), options_(options) {}

    std::vector<webrtc::VideoStream> CreateEncoderStreams(const webrtc::FieldTrialsView&, int width, int height,
                                                          const webrtc::VideoEncoderConfig&) override
    {
        std::vector<webrtc::VideoStream> streams;
        for (auto& p : PlanLayers(layers_, options_, width, height)) {
            webrtc::VideoStream s;
            s.width = p.width;
            s.height = p.height;
            s.max_framerate = p.fps;
            s.min_bitrate_bps = p.minBitrate;
            s.target_bitrate_bps = p.targetBitrate;
            s.max_bitrate_bps = p.maxBitrate;
            s.max_qp = options_.smooth ? 63 : 56;
            s.num_temporal_layers = 1;
            s.bitrate_priority = 4.0;
            s.active = p.active;
            streams.push_back(s);
        }
        return streams;
    }

private:
    std::vector<Layer> layers_;
    Connection::VideoSendOptions options_;
};

webrtc::DegradationPreference Degradation(int discord)
{
    switch (discord) {
    case 0: return webrtc::DegradationPreference::MAINTAIN_RESOLUTION;
    case 1: return webrtc::DegradationPreference::MAINTAIN_FRAMERATE;
    case 3: return webrtc::DegradationPreference::MAINTAIN_FRAMERATE_AND_RESOLUTION;
    default: return webrtc::DegradationPreference::BALANCED;
    }
}

} // namespace

// ------------------------------------------------------------------------------------------ helpers

std::unique_ptr<webrtc::VideoEncoderFactory> CreateVideoEncoderFactory()
{
    return std::make_unique<EncoderFactory>();
}

std::string EncodableCodec(const std::string& name)
{
    if (name == "VP8") return "VP8";
    if (name == "H264" && FfmpegCanEncode(EncodeCodec::H264)) return "H264";
    if (name == "H265" && FfmpegCanEncode(EncodeCodec::H265)) return "H265";
    return "";
}

int VideoFrameEncryptor::Encrypt(webrtc::MediaType, uint32_t ssrc, std::span<const uint8_t>, std::span<const uint8_t> frame,
                                 std::span<uint8_t> out, size_t* written)
{
    Bytes encrypted;
    if (!c_->dave_->Encrypt(discord::dave::MediaType::Video, ssrc, frame.data(), frame.size(), encrypted)) return 1;
    if (encrypted.size() > out.size()) return 2;
    memcpy(out.data(), encrypted.data(), encrypted.size());
    *written = encrypted.size();
    return 0;
}

size_t VideoFrameEncryptor::GetMaxCiphertextByteSize(webrtc::MediaType, size_t frameSize)
{
    return std::max(frameSize + 64, c_->dave_->MaxCiphertextSize(discord::dave::MediaType::Video, frameSize));
}

// ------------------------------------------------------------------------------------------ JS API

void Connection::SetVideoBroadcast(bool broadcast)
{
    VV_LOGI("video", "video broadcast %d (%s)", broadcast, context_.c_str());
    Engine::Get().Worker()->PostTask([this, broadcast] {
        videoBroadcast_ = broadcast;
        UpdateVideoSend();
    });
}

void Connection::SetDesktopSource(const json& options)
{
    auto type = Get<std::string>(options, "type", "");
    auto sourceId = Get<std::string>(options, "sourceId", "");
    VV_LOGI("video", "desktop source: type %s id %s", type.c_str(), sourceId.c_str());
    if (type.empty() && sourceId.empty()) {
        ClearDesktopSource();
        return;
    }
    std::unique_ptr<PipeWireCapturer> capturer;
    webrtc::scoped_refptr<FrameSource> source;
    PickedSource picked;
    if (type == "prepicked" && ScreenCastPortal::Get().TakeCapture(capturer, source)) {
        // already running since the pick
    } else if (type != "prepicked" || !ScreenCastPortal::Get().TakePicked(picked)) {
        // screen: / window: ids are X11 / Windows enumerations; on Wayland everything goes through the portal
        VV_LOGE("video", "desktop source %s:%s is not available (only portal picks are supported)", type.c_str(), sourceId.c_str());
        callbacks.desktopSourceEnded.Call([](Napi::Env env) {
            return JsCallback::Args{Napi::Number::New(env, 1), Napi::Number::New(env, 1)};
        });
        return;
    }
    auto onEnded = [this](const std::string&) {
        // the portal session ended (stopped from the system UI, window closed…): JS stops the stream
        callbacks.desktopSourceEnded.Call([](Napi::Env env) { return JsCallback::Args{Napi::Number::New(env, 0), env.Null()}; });
    };
    if (!capturer) {
        source = webrtc::make_ref_counted<FrameSource>(true, "desktop");
        capturer = std::make_unique<PipeWireCapturer>(picked, source, onEnded);
        if (!capturer->Start()) {
            callbacks.desktopSourceEnded.Call([](Napi::Env env) {
                return JsCallback::Args{Napi::Number::New(env, 1), Napi::Number::New(env, 2)};
            });
            return;
        }
    } else {
        capturer->SetEnded(onEnded);
    }
    std::unique_ptr<PipeWireCapturer> old;
    {
        std::lock_guard lock(desktopMutex_);
        old = std::move(desktopCapturer_);
        desktopCapturer_ = std::move(capturer);
        desktopSource_ = source;
    }
    old.reset();
    Engine::Get().Worker()->PostTask([this] { UpdateVideoSend(); });
}

void Connection::ClearDesktopSource()
{
    std::unique_ptr<PipeWireCapturer> old;
    {
        std::lock_guard lock(desktopMutex_);
        old = std::move(desktopCapturer_);
        desktopSource_ = nullptr;
    }
    // stopping the capture ends the portal session too (one global session)
    if (old) {
        old.reset();
        ScreenCastPortal::Get().CloseSession();
    }
    Engine::Get().Worker()->PostTask([this] { UpdateVideoSend(); });
}

void Connection::SetGoLiveDevices(const json& options)
{
    auto video = Camera::Get().Resolve(Get<std::string>(options, "videoInputDeviceId", ""));
    auto audio = Get<std::string>(options, "audioInputDeviceId", "");
    VV_LOGI("video", "Go Live devices: video %s, audio %s", video.c_str(), audio.c_str());
    if (context_ == "stream") {
        std::lock_guard lock(soundshareMutex_);
        if (!audio.empty() || soundshare_) EnsureSoundshare()->SetDevice(audio);
    }
    Engine::Get().Worker()->PostTask([this, video] {
        goLiveVideoGuid_ = video == "disabled" ? "" : video;
        UpdateVideoSend();
    });
}

void Connection::ClearGoLiveDevices()
{
    VV_LOGI("video", "Go Live devices cleared");
    {
        std::lock_guard lock(soundshareMutex_);
        if (soundshare_) soundshare_->SetDevice("");
    }
    Engine::Get().Worker()->PostTask([this] {
        goLiveVideoGuid_.clear();
        UpdateVideoSend();
    });
}

void Connection::SetVideoSendOptions(const json& o)
{
    bool any = false;
    for (auto* k : {"videoEncoder", "streamParameters", "encodingVideoWidth", "encodingVideoHeight", "encodingVideoFrameRate",
                    "encodingVideoBitRate", "encodingVideoMinBitRate", "encodingVideoMaxBitRate",
                    "encodingVideoDegradationPreference", "remoteSinkWantsPixelCount", "remoteSinkWantsMaxFramerate",
                    "keyframeInterval", "alwaysSendVideo", "vesktopVideoBitrate"})
        any |= o.contains(k);
    if (!any) return;
    Engine::Get().Worker()->PostTask([this, o] {
        auto& v = videoSendOptions_;
        if (o.contains("videoEncoder") && o["videoEncoder"].is_object()) videoEncoder_ = o["videoEncoder"];
        // JS sends the layers it changed (setStreamParameters), not always all of them: merge by rid
        if (o.contains("streamParameters") && o["streamParameters"].is_array()) {
            if (!streamParameters_.is_array()) streamParameters_ = json::array();
            for (auto& p : o["streamParameters"]) {
                bool merged = false;
                for (auto& have : streamParameters_)
                    if (Get<std::string>(have, "type", "") == Get<std::string>(p, "type", "") &&
                        Get<std::string>(have, "rid", "") == Get<std::string>(p, "rid", "")) {
                        have.update(p, true);
                        merged = true;
                        break;
                    }
                if (!merged) streamParameters_.push_back(p);
            }
        }
        v.width = Get(o, "encodingVideoWidth", v.width);
        v.height = Get(o, "encodingVideoHeight", v.height);
        v.fps = Get(o, "encodingVideoFrameRate", v.fps);
        v.bitrate = Get(o, "encodingVideoBitRate", v.bitrate);
        v.minBitrate = Get(o, "encodingVideoMinBitRate", v.minBitrate);
        v.maxBitrate = Get(o, "encodingVideoMaxBitRate", v.maxBitrate);
        v.degradation = Get(o, "encodingVideoDegradationPreference", v.degradation);
        v.sinkWantsPixels = Get(o, "remoteSinkWantsPixelCount", v.sinkWantsPixels);
        v.sinkWantsFps = Get(o, "remoteSinkWantsMaxFramerate", v.sinkWantsFps);
        v.keyframeIntervalMs = Get(o, "keyframeInterval", v.keyframeIntervalMs);
        v.alwaysSend = Get(o, "alwaysSendVideo", v.alwaysSend);
        if (o.contains("vesktopVideoBitrate")) {
            v.bitrateOverride = std::max(0, Get(o, "vesktopVideoBitrate", 0));
            VV_LOGI("video", "video bitrate override: %d", v.bitrateOverride);
        }
        UpdateVideoSend();
    });
}

// ------------------------------------------------------------------------------------------ worker

void Connection::UpdateVideoSend()
{
    if (!call_) return;
    bool stream = context_ == "stream";
    if (videoBroadcast_ && !videoSendStream_ && streamParameters_.is_array())
        // JS starts every layer as `active: false` and takes the flag back from our setOnVideoCallback report
        // (handleVideo), so a new stream has to start with its layers active. The
        // encoder still stays idle until viewers ask for pixels
        for (auto& p : streamParameters_)
            if (Get<std::string>(p, "type", "") == "video") p["active"] = true;
    auto layers = VideoLayers(streamParameters_);
    auto codecName = Get<std::string>(videoEncoder_, "name", "");
    int payloadType = Get(videoEncoder_, "type", 0);
    int rtxPayloadType = Get(videoEncoder_, "rtxType", 0);

    webrtc::scoped_refptr<FrameSource> source;
    std::string cameraGuid; // the camera this connection sends, "" = the selected one
    bool usesCamera = !stream;
    if (stream) {
        std::lock_guard lock(desktopMutex_);
        source = desktopSource_;
    }
    if (stream && !source && !goLiveVideoGuid_.empty()) {
        // Go Live with a device (setGoLiveDevices)
        cameraGuid = goLiveVideoGuid_;
        usesCamera = true;
    }
    if (usesCamera) source = Camera::Get().Source(cameraGuid);

    bool want = videoBroadcast_ && source && !layers.empty() && !codecName.empty() && payloadType > 0;
    if (want && EncodableCodec(codecName).empty()) {
        // JS drops the codec and renegotiates (setOnVideoEncoderFallbackCallback)
        if (encoderFallbacksReported_.insert(codecName).second) {
            VV_LOGW("video", "cannot encode %s, asking for a fallback", codecName.c_str());
            callbacks.encoderFallback.Call([codecName](Napi::Env env) { return JsCallback::Args{Napi::String::New(env, codecName)}; });
        }
        want = false;
    }

    // the camera captures only while a connection broadcasts it
    bool needCamera = want && usesCamera;
    if (needCamera) Camera::Get().Acquire(this, videoSendOptions_.width, videoSendOptions_.height, videoSendOptions_.fps, cameraGuid);
    else if (cameraAcquired_) Camera::Get().Release(this);
    cameraAcquired_ = needCamera;
    // a Go Live camera also previews as the stream (ReportSelfVideo sets the id)
    if (goLivePreviewGuid_ != (needCamera && stream ? cameraGuid : "")) {
        if (auto old = Camera::Get().Source(goLivePreviewGuid_); old && !goLivePreviewGuid_.empty()) old->SetStreamPreviewId("");
        goLivePreviewGuid_ = needCamera && stream ? cameraGuid : "";
    }

    if (!want) {
        DestroyVideoSendStream();
        return;
    }

    if (!encoderFactory_) {
        encoderFactory_ = CreateVideoEncoderFactory();
        bitrateAllocatorFactory_ = webrtc::CreateBuiltinVideoBitrateAllocatorFactory();
        videoEncryptor_ = webrtc::make_ref_counted<VideoFrameEncryptor>(this);
        videoCname_ = RandomCname();
    }

    json key = {{"codec", codecName}, {"pt", payloadType}, {"rtx", rtxPayloadType}, {"source", reinterpret_cast<uintptr_t>(source.get())}};
    for (auto& l : layers) key["ssrcs"].push_back({l.ssrc, l.rtxSsrc});
    if (videoSendStream_ && key != videoSendKey_) DestroyVideoSendStream();

    // encoder config
    auto& o = videoSendOptions_;
    o.screen = stream;
    webrtc::VideoEncoderConfig ec;
    const std::string codec = EncodableCodec(codecName); // VP8, H264 or H265
    if (codec == "H264") {
        ec.codec_type = webrtc::kVideoCodecH264;
        ec.video_format = webrtc::SdpVideoFormat("H264", {{"packetization-mode", "1"}, {"level-asymmetry-allowed", "1"}});
    } else if (codec == "H265") {
        ec.codec_type = webrtc::kVideoCodecH265;
        ec.video_format = webrtc::SdpVideoFormat("H265");
    } else {
        ec.codec_type = webrtc::kVideoCodecVP8;
        ec.video_format = webrtc::SdpVideoFormat("VP8");
    }
    bool screenContent = stream && !usesCamera;
    // Go Live "Prefer Smoothness" (any degradation but MAINTAIN_RESOLUTION): keep the frame rate when bits run
    // short and let the picture blur instead, as for a camera. libvpx's screen content mode (and the frame
    // dropper) do the opposite: a sharp image at whatever frame rate the bitrate allows ("Prefer Clarity")
    bool smooth = screenContent && o.degradation != 0;
    o.smooth = smooth;
    if (smooth) screenContent = false;
    ec.content_type = screenContent ? webrtc::VideoEncoderConfig::ContentType::kScreen : webrtc::VideoEncoderConfig::ContentType::kRealtimeVideo;
    ec.number_of_streams = layers.size();
    ec.simulcast_layers.resize(layers.size());
    ec.video_stream_factory = webrtc::make_ref_counted<LayerStreamFactory>(layers, o);
    ec.min_transmit_bitrate_bps = o.minBitrate > 0 ? o.minBitrate : kDefaultMinBitrate;
    ec.max_bitrate_bps = 0;
    for (auto& p : PlanLayers(layers, o, 1280, 720)) ec.max_bitrate_bps += p.maxBitrate;
    ec.bitrate_priority = 4.0;
    ec.frame_drop_enabled = !smooth;
    ec.is_quality_scaling_allowed = smooth; // lower the resolution (blur) instead of the frame rate
    ec.max_qp = smooth ? 63 : 56;
    webrtc::VideoCodecVP8 vp8 = webrtc::VideoEncoder::GetDefaultVp8Settings();
    vp8.numberOfTemporalLayers = 1;
    vp8.denoisingOn = !screenContent && !smooth;
    vp8.automaticResizeOn = false;
    int fps = MinPositive({o.fps}, 30);
    vp8.keyFrameInterval = o.keyframeIntervalMs > 0 ? std::max(1, o.keyframeIntervalMs * fps / 1000) : 3000;
    if (codec == "VP8") ec.encoder_specific_settings = webrtc::make_ref_counted<webrtc::VideoEncoderConfig::Vp8EncoderSpecificSettings>(vp8);

    json encoderKey = {{"layers", streamParameters_}, {"w", o.width}, {"h", o.height}, {"fps", o.fps}, {"b", o.bitrate},
                       {"min", o.minBitrate}, {"max", o.maxBitrate}, {"deg", o.degradation}, {"px", o.sinkWantsPixels},
                       {"pxfps", o.sinkWantsFps}, {"kf", o.keyframeIntervalMs}, {"always", o.alwaysSend},
                       {"override", o.bitrateOverride}};
    source->SetOutputFormat(o.width, o.height, o.fps);

    if (!videoSendStream_) {
        webrtc::VideoSendStream::Config config(RtpTransport());
        for (auto& l : layers) {
            config.rtp.ssrcs.push_back(l.ssrc);
            if (l.rtxSsrc && rtxPayloadType > 0) config.rtp.rtx.ssrcs.push_back(l.rtxSsrc);
            dave_->AssignSsrc(l.ssrc, codec == "H264" ? discord::dave::Codec::H264
                                      : codec == "H265" ? discord::dave::Codec::H265
                                                        : discord::dave::Codec::VP8);
        }
        if (config.rtp.rtx.ssrcs.size() != config.rtp.ssrcs.size()) config.rtp.rtx.ssrcs.clear();
        else config.rtp.rtx.payload_type = rtxPayloadType;
        config.rtp.payload_name = codec;
        config.rtp.payload_type = payloadType;
        config.rtp.c_name = videoCname_;
        config.rtp.max_packet_size = 1200;
        config.rtp.rtcp_mode = webrtc::RtcpMode::kCompound;
        config.rtp.nack.rtp_history_ms = 1000;
        // the extensions the SFU expects on video
        using E = webrtc::RtpExtension;
        auto ext = [](const char* uri, int id) { return E(uri, webrtc::RtpHeaderExtensionId(id)); };
        config.rtp.extensions = {ext(E::kTimestampOffsetUri, 2),      ext(E::kAbsSendTimeUri, 3),
                                 ext(E::kVideoRotationUri, 4),        ext(E::kTransportSequenceNumberUri, 5),
                                 ext(E::kPlayoutDelayUri, 6),         ext(E::kVideoContentTypeUri, 7),
                                 ext(E::kVideoTimingUri, 8)};
        config.rtcp_report_interval_ms = 1000;
        config.encoder_settings.encoder_factory = encoderFactory_.get();
        config.encoder_settings.bitrate_allocator_factory = bitrateAllocatorFactory_.get();
        config.frame_encryptor = videoEncryptor_;
        // Never pause the stream when the estimate falls below the layer's minimum: the SFU acknowledges nothing
        // while nobody watches, the estimate then drops to the floor, and a suspended stream had nothing for the
        // viewer who arrives next (a window stream that stayed black). The encoder keeps its minimum bitrate instead
        config.suspend_below_min_bitrate = false;
        // A screen rarely needs all the bits it may use, and an application-limited sender never raises its
        // estimate without probing: probe periodically so a higher bitrate becomes available when it is needed
        config.periodic_alr_bandwidth_probing = stream;
        videoSendStream_ = call_->CreateVideoSendStream(std::move(config), std::move(ec));
        videoSendStream_->SetSource(source.get(), Degradation(o.degradation));
        videoSendStream_->Start();
        videoStatsTask_ = webrtc::RepeatingTaskHandle::DelayedStart(Engine::Get().Worker(), webrtc::TimeDelta::Seconds(3), [this] {
            if (!videoSendStream_) return webrtc::TimeDelta::PlusInfinity();
            auto s = videoSendStream_->GetStats();
            int w = 0, h = 0;
            for (auto& [ssrc, sub] : s.substreams)
                if (sub.width * sub.height > w * h) w = sub.width, h = sub.height;
            VV_LOGI("video", "send: %u frames encoded, %dx%d, %d kbit/s media (target %d), %.0f fps in / %d encoded, source %llu frames, wants %d px, %s%s%s%s",
                    s.frames_encoded, w, h, s.media_bitrate_bps / 1000, s.target_media_bitrate_bps / 1000, s.input_frame_rate,
                    s.encode_frame_rate, static_cast<unsigned long long>(videoSendSource_ ? videoSendSource_->FramesPushed() : 0),
                    videoSendOptions_.sinkWantsPixels, videoSendOptions_.smooth ? "smooth" : "sharp",
                    s.suspended ? ", SUSPENDED" : "", s.bw_limited_resolution ? ", bw limited" : "",
                    s.cpu_limited_resolution ? ", cpu limited" : "");
            return webrtc::TimeDelta::Seconds(5);
        });
        videoSendKey_ = key;
        videoEncoderKey_ = encoderKey;
        videoSendSource_ = source;
        VV_LOGI("video", "video send stream: %s (%s) pt %d rtx %d, %zu layers, ssrc %u, %s", codec.c_str(),
                codec == "VP8" ? "libvpx" : FfmpegEncoderName(codec == "H264" ? EncodeCodec::H264 : EncodeCodec::H265).c_str(), payloadType,
                rtxPayloadType, layers.size(), layers.back().ssrc, smooth ? "screen (smooth)" : screenContent ? "screen" : stream ? "Go Live camera" : "camera");
        for (auto& p : PlanLayers(layers, o, 1280, 720))
            VV_LOGI("video", "layer: max %d kbit/s, encoder %s (sink wants %d px, alwaysSend %d)", p.maxBitrate / 1000,
                    p.active ? "active" : "idle until viewers ask for pixels", o.sinkWantsPixels, o.alwaysSend);
    } else if (encoderKey != videoEncoderKey_) {
        VV_LOGI("video", "video encoder reconfigured: %s, degradation %d", smooth ? "screen (smooth)" : screenContent ? "screen" : stream ? "Go Live camera" : "camera",
                o.degradation);
        videoSendStream_->ReconfigureVideoEncoder(std::move(ec));
        videoSendStream_->SetSource(source.get(), Degradation(o.degradation));
        videoEncoderKey_ = encoderKey;
    }

    // tell JS which layers run (setOnVideoCallback for our own user)
    int w = source->LastWidth() > 0 ? source->LastWidth() : 1280, h = source->LastHeight() > 0 ? source->LastHeight() : 720;
    auto plans = PlanLayers(layers, o, w, h);
    json streams = json::array();
    for (size_t i = 0; i < layers.size(); i++) {
        auto& l = layers[i];
        // `active` is the layer's own flag from JS, not whether the encoder runs right now: it goes to the gateway
        // (VIDEO op), and the SFU only forwards streams marked active. The encoder layer itself stays idle until
        // viewers ask for pixels (sink wants) or alwaysSendVideo is set
        streams.push_back({{"type", "video"}, {"rid", l.rid}, {"ssrc", l.ssrc}, {"rtxSsrc", l.rtxSsrc}, {"active", l.active},
                           {"maxBitrate", plans[i].maxBitrate}, {"minBitrate", plans[i].minBitrate},
                           {"targetBitrate", plans[i].targetBitrate}, {"quality", l.quality},
                           {"maxPixelCount", plans[i].width * plans[i].height}, {"maxFrameRate", plans[i].fps},
                           {"soundshare", false}});
    }
    ReportSelfVideo(layers.back().ssrc, streams);
}

void Connection::DestroyVideoSendStream()
{
    videoStatsTask_.Stop();
    if (videoSendStream_) {
        videoSendStream_->Stop();
        videoSendStream_->SetSource(nullptr, webrtc::DegradationPreference::BALANCED);
        call_->DestroyVideoSendStream(videoSendStream_);
        videoSendStream_ = nullptr;
        videoSendKey_ = nullptr;
        videoEncoderKey_ = nullptr;
        videoSendSource_ = nullptr;
        VV_LOGI("video", "video send stream destroyed");
    }
    ReportSelfVideo(0, json::array());
}

void Connection::ReportSelfVideo(uint32_t ssrc, const json& streams)
{
    std::string streamId;
    if (ssrc) {
        if (context_ == "stream") {
            streamId = std::to_string(ssrc);
            std::lock_guard lock(desktopMutex_);
            if (desktopSource_) desktopSource_->SetPreviewStreamId(streamId);
            else if (!goLivePreviewGuid_.empty())
                if (auto camera = Camera::Get().Source(goLivePreviewGuid_)) camera->SetStreamPreviewId(streamId);
        } else {
            streamId = Camera::Get().DeviceGuid();
        }
    }
    json report = {{"ssrc", ssrc}, {"streamId", streamId}, {"streams", streams}};
    if (report == lastSelfVideo_) return;
    if (lastSelfVideo_.is_null() && !ssrc) return; // nothing was ever reported
    lastSelfVideo_ = report;
    auto id = userId_;
    VV_LOGI("video", "own video: ssrc %u stream %s, %zu layers", ssrc, streamId.c_str(), streams.size());
    callbacks.video.Call([id, ssrc, streamId, streams](Napi::Env env) {
        return JsCallback::Args{Napi::String::New(env, id), Napi::Number::New(env, ssrc), Napi::String::New(env, streamId),
                                FromJson(env, streams)};
    });
}

// outbound.videos[]
json Connection::OutboundVideoStats()
{
    json videos = json::array();
    if (!videoSendStream_) return videos;
    auto s = videoSendStream_->GetStats();
    auto counters = [](const webrtc::RtpPacketCounter& c) {
        return json{{"headerBytes", c.header_bytes}, {"payloadBytes", c.payload_bytes}, {"paddingBytes", c.padding_bytes},
                    {"packets", c.packets}};
    };
    json substreams = json::array();
    for (auto& [ssrc, ss] : s.substreams) {
        json sub = {{"ssrc", ssrc},
                    {"isRTX", ss.type == webrtc::VideoSendStream::StreamStats::StreamType::kRtx},
                    {"isFlexFEC", ss.type == webrtc::VideoSendStream::StreamStats::StreamType::kFlexfec},
                    {"width", ss.width},
                    {"height", ss.height},
                    {"frameCounts", {{"keyFrames", ss.frame_counts.key_frames}, {"deltaFrames", ss.frame_counts.delta_frames}}},
                    {"rtpStats", {{"transmitted", counters(ss.rtp_stats.transmitted)},
                                  {"retransmitted", counters(ss.rtp_stats.retransmitted)},
                                  {"fec", counters(ss.rtp_stats.fec)}}},
                    {"totalBitrate", ss.total_bitrate_bps},
                    {"retransmitBitrate", ss.retransmit_bitrate_bps},
                    {"avgDelay", ss.avg_delay_ms},
                    {"maxDelay", ss.max_delay_ms},
                    {"qpSum", ss.qp_sum.value_or(0)}};
        if (ss.report_block_data)
            sub["rtcpStats"] = {{"packetsLost", ss.report_block_data->cumulative_lost()},
                                {"fractionLost", ss.report_block_data->fraction_lost()},
                                {"firPackets", ss.rtcp_packet_type_counts.fir_packets},
                                {"nackPackets", ss.rtcp_packet_type_counts.nack_packets},
                                {"pliPackets", ss.rtcp_packet_type_counts.pli_packets}};
        substreams.push_back(sub);
    }
    auto enc = dave_->EncryptorStats(discord::dave::MediaType::Video);
    uint32_t ssrc = s.substreams.empty() ? 0 : s.substreams.begin()->first;
    for (auto& [id, ss] : s.substreams)
        if (ss.type == webrtc::VideoSendStream::StreamStats::StreamType::kMedia) ssrc = id;
    videos.push_back({{"ssrc", ssrc},
                      {"substreams", substreams},
                      {"codecPayloadType", Get(videoEncoder_, "type", 0)},
                      {"codecName", Get<std::string>(videoEncoder_, "name", "")},
                      {"keyFrameInterval", 0},
                      {"mediaBitrate", s.media_bitrate_bps},
                      {"targetMediaBitrate", s.target_media_bitrate_bps},
                      {"encoderImplementationName", s.encoder_implementation_name.value_or("")},
                      {"encodeUsage", s.encode_usage_percent},
                      {"avgEncodeTime", s.avg_encode_time_ms},
                      {"framesEncoded", s.frames_encoded},
                      {"inputFrameRate", s.input_frame_rate},
                      {"encodeFrameRate", s.encode_frame_rate},
                      {"bwLimitedResolution", s.bw_limited_resolution},
                      {"bwLimitedFrameRate", s.bw_limited_framerate},
                      {"cpuLimitedResolution", s.cpu_limited_resolution},
                      {"cpuLimitedFrameRate", s.cpu_limited_framerate},
                      {"numberOfCPUAdaptChanges", s.number_of_cpu_adapt_changes},
                      {"numberOfQualityAdaptChanges", s.number_of_quality_adapt_changes},
                      {"suspended", s.suspended},
                      {"framesDroppedRateLimiter", s.frames_dropped_by_rate_limiter},
                      {"framesDroppedEncoderQueue", s.frames_dropped_by_encoder_queue},
                      {"framesDroppedCongestionWindow", s.frames_dropped_by_congestion_window},
                      {"framesDroppedEncoder", s.frames_dropped_by_encoder},
                      {"filter", ""},
                      {"passthroughCount", enc.passthroughCount},
                      {"encryptSuccessCount", enc.encryptSuccessCount},
                      {"encryptFailureCount", enc.encryptFailureCount},
                      {"encryptDuration", enc.encryptDuration},
                      {"encryptAttempts", enc.encryptAttempts},
                      {"encryptMaxAttempts", enc.encryptMaxAttempts},
                      {"encryptMissingKeyCount", enc.encryptMissingKeyCount}});
    return videos;
}

} // namespace vv
