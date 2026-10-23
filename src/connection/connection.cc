#include "connection/connection.h"

#include <api/audio_codecs/builtin_audio_decoder_factory.h>
#include <api/audio_codecs/builtin_audio_encoder_factory.h>
#include <api/call/transport.h>
#include <api/crypto/frame_decryptor_interface.h>
#include <api/crypto/frame_encryptor_interface.h>
#include <api/make_ref_counted.h>
#include <api/media_types.h>
#include <api/rtp_parameters.h>
#include <api/units/timestamp.h>
#include <call/audio_receive_stream.h>
#include <call/audio_send_stream.h>
#include <call/call.h>
#include <call/audio_state.h>
#include <call/call_config.h>
#include <modules/audio_device/include/fake_audio_device.h>
#include <call/video_receive_stream.h>
#include <api/video/video_frame.h>
#include <api/video/video_sink_interface.h>
#include <api/video_codecs/sdp_video_format.h>
#include <modules/rtp_rtcp/source/rtp_packet_received.h>
#include <rtc_base/copy_on_write_buffer.h>
#include <rtc_base/network/sent_packet.h>
#include <rtc_base/thread.h>
#include <rtc_base/time_utils.h>

#include <poll.h>

#include <algorithm>
#include <cmath>
#include <random>

#include "audio/mixer.h"
#include "audio/soundshare.h"
#include "common/log.h"
#include "common/util.h"
#include "capture/camera.h"
#include "capture/portal.h"
#include "capture/pipewire_capturer.h"
#include "connection/video_send.h"
#include "video/decoder_factory.h"
#include "video/frame_source.h"
#include "video/video_sinks.h"

namespace vv {

namespace {

constexpr int kAudioPayloadType = 120;
constexpr uint8_t kOpusSilence[] = {0xf8, 0xff, 0xfe};
// 10 Opus silence frames once encryption is set and whenever sending stops
constexpr int kTrailingSilenceFrames = 10;

std::string RandomCname()
{
    static const char chars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::random_device rd;
    std::string s;
    for (int i = 0; i < 16; i++) s += chars[rd() % (sizeof(chars) - 1)];
    return s;
}

bool IsRtcp(const uint8_t* p, size_t n)
{
    return n >= 8 && (p[0] >> 6) == 2 && p[1] >= 200 && p[1] <= 206;
}

} // namespace

// ------------------------------------------------------------------------------------------ MediaTransport

// webrtc::Transport: every outgoing RTP / RTCP packet gets the transport encryption and goes to the SFU
class MediaTransport : public webrtc::Transport {
public:
    explicit MediaTransport(Connection* c) : c_(c) {}
    bool SendRtp(std::span<const uint8_t> packet, const webrtc::PacketOptions& options) override
    {
        bool video = packet.size() >= 12 && c_->ssrc_ != ReadU32(packet.data() + 8);
        size_t wire = 0;
        bool sent = c_->SendMedia(packet.data(), packet.size(), false, video, &wire);
        // Tell the congestion controller what left and when, as a network transport does: transport-cc feedback
        // from the SFU is matched against these send times. Without them every feedback fails ("Failed to lookup
        // send time"), the delay-based estimate never grows and video stays at about 4 Mbit/s whatever is allowed
        if (sent && options.packet_id >= 0) {
            webrtc::PacketInfo info;
            info.included_in_feedback = options.included_in_feedback;
            info.included_in_allocation = options.included_in_allocation;
            info.is_media = options.is_media;
            info.packet_type = webrtc::PacketType::kData;
            info.protocol = webrtc::PacketInfoProtocolType::kUdp;
            info.packet_size_bytes = wire;
            info.ip_overhead_bytes = 28; // IPv4 + UDP
            if (auto* call = c_->call_.get()) call->OnSentPacket(webrtc::SentPacketInfo(options.packet_id, webrtc::TimeMillis(), info));
        }
        return sent;
    }
    bool SendRtcp(std::span<const uint8_t> packet, const webrtc::PacketOptions&) override
    {
        return c_->SendMedia(packet.data(), packet.size(), true, false);
    }

private:
    Connection* c_;
};

// ------------------------------------------------------------------------------------------ FrameGate

// Frame encryptor of our audio send stream. Two jobs:
//  * the voice activity / push to talk / mute gate: while closed, frames are dropped before packetization
//    (so no RTP sequence numbers are used), after sending a few Opus silence frames;
//  * DAVE end-to-end encryption of the encoded frame.
class FrameGate : public webrtc::FrameEncryptorInterface {
public:
    explicit FrameGate(Connection* c) : c_(c) {}

    void SetOpen(bool open)
    {
        bool was = open_.exchange(open);
        if (was && !open) trailing_ = kTrailingSilenceFrames;
    }
    void SendSilence() { trailing_ = kTrailingSilenceFrames; }

    int Encrypt(webrtc::MediaType type, uint32_t ssrc, std::span<const uint8_t>, std::span<const uint8_t> frame,
                std::span<uint8_t> out, size_t* written) override
    {
        const uint8_t* data = frame.data();
        size_t size = frame.size();
        if (!open_) {
            if (trailing_ <= 0) return 1; // dropped
            trailing_--;
            data = kOpusSilence;
            size = sizeof(kOpusSilence);
        }
        Bytes encrypted;
        auto mediaType = type == webrtc::MediaType::VIDEO ? discord::dave::MediaType::Video : discord::dave::MediaType::Audio;
        if (!c_->dave_->Encrypt(mediaType, ssrc, data, size, encrypted) || encrypted.size() > out.size()) return 2;
        memcpy(out.data(), encrypted.data(), encrypted.size());
        *written = encrypted.size();
        return 0;
    }

    size_t GetMaxCiphertextByteSize(webrtc::MediaType, size_t frameSize) override { return frameSize + 64; }

private:
    Connection* c_;
    std::atomic<bool> open_{false};
    std::atomic<int> trailing_{0};
};

// ------------------------------------------------------------------------------------------ FrameDecryptor

// DAVE decryption of one remote user's frames (passthrough while DAVE is off)
class FrameDecryptor : public webrtc::FrameDecryptorInterface {
public:
    FrameDecryptor(DaveSession* dave, std::string userId) : dave_(dave), userId_(std::move(userId)) {}

    Result Decrypt(webrtc::MediaType type, const std::vector<uint32_t>&, std::span<const uint8_t>,
                   std::span<const uint8_t> encrypted, std::span<uint8_t> frame) override
    {
        Bytes out;
        auto mediaType = type == webrtc::MediaType::VIDEO ? discord::dave::MediaType::Video : discord::dave::MediaType::Audio;
        if (!dave_->Decrypt(userId_, mediaType, encrypted.data(), encrypted.size(), out) || out.size() > frame.size())
            return Result(Status::kFailedToDecrypt, 0);
        memcpy(frame.data(), out.data(), out.size());
        return Result(Status::kOk, out.size());
    }

    size_t GetMaxPlaintextByteSize(webrtc::MediaType, size_t size) override { return size; }

private:
    DaveSession* dave_;
    std::string userId_;
};

// ------------------------------------------------------------------------------------------ RemoteVideoSink

// Renderer of one remote video stream: first-frame callback, then frames to the JS sink of its streamId
class RemoteVideoSink : public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
public:
    RemoteVideoSink(Connection* c, std::string userId, uint32_t ssrc)
        : c_(c), userId_(std::move(userId)), ssrc_(ssrc), streamId_(std::to_string(ssrc)) {}

    void OnFrame(const webrtc::VideoFrame& frame) override
    {
        if (!firstFrame_.exchange(true)) {
            VV_LOGI("video", "first frame from %s ssrc %u: %dx%d", userId_.c_str(), ssrc_, frame.width(), frame.height());
            c_->callbacks.firstFrame.Call([id = userId_, ssrc = ssrc_, streamId = streamId_](Napi::Env env) {
                return JsCallback::Args{Napi::String::New(env, id), Napi::Number::New(env, ssrc), Napi::String::New(env, streamId)};
            });
        }
        VideoSinks::Get().Deliver(streamId_, frame);
    }

    bool Rendered() const { return firstFrame_; }

private:
    Connection* c_;
    const std::string userId_;
    const uint32_t ssrc_;
    const std::string streamId_;
    std::atomic<bool> firstFrame_{false};
};

// ------------------------------------------------------------------------------------------ Connection

Connection::Connection(std::string userId, const json& options, JsCallbackPtr onConnect)
    : userId_(std::move(userId)), ssrc_(Get<uint32_t>(options, "ssrc", 0)), onConnect_(std::move(onConnect))
{
    createdMs_ = NowMs();
    context_ = Get<std::string>(options, "context", "default");
    streamUserId_ = Get<std::string>(options, "streamUserId", "");
    signingKeyId_ = Get<std::string>(options, "signingKeyId", "");
    streamParameters_ = options.value("streamParameters", json::array());
    auto address = Get<std::string>(options, "address", "");
    int port = Get(options, "port", 0);
    VV_LOGI("conn", "VoiceConnection user %s ssrc %u %s:%d context %s", userId_.c_str(), ssrc_, address.c_str(), port,
            context_.c_str());

    // Header extensions we parse
    extensions_.RegisterByUri(webrtc::RtpHeaderExtensionId(1), webrtc::RtpExtension::kAudioLevelUri);
    extensions_.RegisterByUri(webrtc::RtpHeaderExtensionId(2), webrtc::RtpExtension::kTimestampOffsetUri);
    extensions_.RegisterByUri(webrtc::RtpHeaderExtensionId(3), webrtc::RtpExtension::kAbsSendTimeUri);
    extensions_.RegisterByUri(webrtc::RtpHeaderExtensionId(4), webrtc::RtpExtension::kVideoRotationUri);
    extensions_.RegisterByUri(webrtc::RtpHeaderExtensionId(5), webrtc::RtpExtension::kTransportSequenceNumberUri);
    extensions_.RegisterByUri(webrtc::RtpHeaderExtensionId(6), webrtc::RtpExtension::kPlayoutDelayUri);
    extensions_.RegisterByUri(webrtc::RtpHeaderExtensionId(7), webrtc::RtpExtension::kVideoContentTypeUri);
    extensions_.RegisterByUri(webrtc::RtpHeaderExtensionId(8), webrtc::RtpExtension::kVideoTimingUri);
    extensions_.RegisterByUri(webrtc::RtpHeaderExtensionId(10), webrtc::RtpExtension::kMidUri);
    extensions_.RegisterByUri(webrtc::RtpHeaderExtensionId(11), webrtc::RtpExtension::kRidUri);
    extensions_.RegisterByUri(webrtc::RtpHeaderExtensionId(12), webrtc::RtpExtension::kRepairedRidUri);

    dave_ = std::make_unique<DaveSession>(
        userId_, signingKeyId_,
        [this](const std::string& source, const std::string& reason) {
            callbacks.mlsFailure.Call([source, reason](Napi::Env env) {
                return JsCallback::Args{Napi::String::New(env, source), Napi::String::New(env, reason)};
            });
        },
        [this](const DaveSession::State& state) {
            callbacks.secureFramesState.Call([state](Napi::Env env) {
                auto o = Napi::Object::New(env);
                o.Set("version", state.version);
                o.Set("epochAuthenticator", state.epochAuthenticator);
                return JsCallback::Args{o};
            });
        });
    dave_->AssignSsrc(ssrc_, discord::dave::Codec::Opus);

    gate_ = webrtc::make_ref_counted<FrameGate>(this);
    decoderFactory_ = std::make_unique<DecoderFactory>();
    transport_ = std::make_unique<MediaTransport>(this);

    auto resolved = SocketAddress::Resolve(address, port);
    if (resolved) {
        remote_ = originalRemote_ = *resolved;
        socket_.Open(resolved->storage.ss_family);
        if (Get(options, "qosEnabled", false)) socket_.SetDscp(46);
    }

    auto& engine = Engine::Get();
    engine.Worker()->BlockingCall([this] { CreateCall(); });
    engine.AddConnection(this);

    if (!resolved) {
        VV_LOGE("conn", "cannot resolve %s", address.c_str());
        if (onConnect_) {
            onConnect_->Call([](Napi::Env env) {
                return JsCallback::Args{Napi::String::New(env, "Failed to resolve address"), env.Null()};
            });
        }
        return;
    }
    running_ = true;
    networkThread_ = std::thread([this] { NetworkLoop(); });
}

Connection::~Connection()
{
    Destroy();
}

void Connection::Destroy()
{
    if (destroyed_.exchange(true)) return;
    VV_LOGI("conn", "destroy %s", userId_.c_str());
    auto& engine = Engine::Get();
    engine.RemoveConnection(this);
    running_ = false;
    if (networkThread_.joinable()) networkThread_.join();
    {
        std::lock_guard lock(soundshareMutex_);
        soundshare_.reset();
    }
    {
        std::lock_guard lock(desktopMutex_);
        bool had = desktopCapturer_ != nullptr;
        desktopCapturer_.reset();
        desktopSource_ = nullptr;
        if (had) ScreenCastPortal::Get().CloseSession();
    }
    engine.Worker()->BlockingCall([this] {
        audioStatsTask_.Stop();
        remoteSpeakingTask_.Stop();
        DestroyVideoSendStream();
        if (cameraAcquired_) Camera::Get().Release(this);
        cameraAcquired_ = false;
        if (!goLivePreviewGuid_.empty())
            if (auto camera = Camera::Get().Source(goLivePreviewGuid_)) camera->SetStreamPreviewId("");
        goLivePreviewGuid_.clear();
        std::lock_guard lock(usersMutex_);
        for (auto& [id, user] : users_) {
            DestroyReceiveStream(user);
            DestroyVideoStreams(user);
        }
        if (sendStream_) {
            sendStream_->Stop();
            call_->DestroyAudioSendStream(sendStream_);
            sendStream_ = nullptr;
        }
        call_.reset();
    });
    engine.Worker()->PostTask([] { Engine::Get().UpdateAudioRunning(); });
    socket_.Close();
}

webrtc::Transport* Connection::RtpTransport()
{
    return transport_.get();
}

void Connection::CreateCall()
{
    auto& engine = Engine::Get();
    auto config = webrtc::CallConfig::CreateWithJoinedWorkerAndNetworkQueue(engine.Env(), engine.Worker());
    if (context_ == "stream") {
        // A stream connection sends soundshare, never the microphone: an AudioState of its own with a device
        // module that captures nothing, fed by OnSoundshareFrame. Received stream audio still goes to the
        // shared playout mixer
        static auto* noDevice = new webrtc::FakeAudioDeviceModule();
        webrtc::AudioState::Config audio;
        audio.audio_mixer = webrtc::scoped_refptr<webrtc::AudioMixer>(&engine.PlayoutMixer());
        audio.audio_device_module = webrtc::scoped_refptr<webrtc::AudioDeviceModule>(noDevice);
        streamAudioState_ = webrtc::AudioState::Create(audio);
        config.audio_state = streamAudioState_;
    } else {
        config.audio_state = engine.AudioState();
    }
    // callBitRate / callMinBitRate / callMaxBitRate defaults
    config.bitrate_config.min_bitrate_bps = 16000;
    config.bitrate_config.start_bitrate_bps = 700000;
    config.bitrate_config.max_bitrate_bps = 10000000;
    if (context_ == "stream") {
        // Go Live: starting at a voice call's 700 kbit/s, the first half minute of a stream was blurred (and
        // downscaled) while the estimate crept up. The initial probes (3x, 6x the start) now check 7.5 and
        // 15 Mbit/s right away; the ceiling is the encoder's (and the SFU's REMB), not the call's 10 Mbit/s
        config.bitrate_config.start_bitrate_bps = 2500000;
        config.bitrate_config.max_bitrate_bps = 50000000;
    }
    call_ = webrtc::Call::Create(std::move(config));
    call_->SignalChannelNetworkState(webrtc::MediaType::AUDIO, webrtc::kNetworkUp);
    call_->SignalChannelNetworkState(webrtc::MediaType::VIDEO, webrtc::kNetworkUp);
    ConfigureSendStream();
    // Discord's client learns who is speaking from the module (the speaking callback, for every user): the gateway
    // only tells it a user's flags. With the browser's WebRTC it measures the received audio itself
    remoteSpeakingTask_ = webrtc::RepeatingTaskHandle::DelayedStart(engine.Worker(), webrtc::TimeDelta::Millis(50), [this] {
        if (!call_) return webrtc::TimeDelta::PlusInfinity();
        UpdateRemoteSpeaking();
        return webrtc::TimeDelta::Millis(50);
    });
    // A call where nobody hears anybody: whether packets flow at all, in each direction
    audioStatsTask_ = webrtc::RepeatingTaskHandle::DelayedStart(engine.Worker(), webrtc::TimeDelta::Seconds(10), [this] {
        if (!call_ || !sendStream_) return webrtc::TimeDelta::PlusInfinity();
        auto sent = sendStream_->GetStats().packets_sent;
        std::string line = "sent " + std::to_string(sent - lastAudioSent_);
        lastAudioSent_ = sent;
        {
            std::lock_guard lock(usersMutex_);
            for (auto& [id, user] : users_) {
                if (!user.stream) continue;
                auto received = static_cast<uint32_t>(user.stream->GetStats(false).packets_received);
                line += ", from " + id + " " + std::to_string(received - lastAudioReceived_[id]);
                lastAudioReceived_[id] = received;
            }
        }
        // what reached the socket: nothing at all = the server sends nothing; failures = we drop it
        uint64_t rtp = rtpIn_, rtcp = rtcpIn_, dec = decryptionFailures_, route = routingFailures_;
        line += "; udp in: " + std::to_string(rtp - lastRtpIn_) + " rtp, " + std::to_string(rtcp - lastRtcpIn_) +
                " rtcp, " + std::to_string(dec - lastDecryptionFailures_) + " undecryptable, " +
                std::to_string(route - lastRoutingFailures_) + " unknown ssrc";
        lastRtpIn_ = rtp;
        lastRtcpIn_ = rtcp;
        lastDecryptionFailures_ = dec;
        lastRoutingFailures_ = route;
        uint64_t ps = pingsSent_, pe = pingsEchoed_, pt = pingsTimedOut_, sh = shortPacketsIn_;
        line += "; pings: " + std::to_string(ps - lastPingsSent_) + " sent, " + std::to_string(pe - lastPingsEchoed_) +
                " echoed, " + std::to_string(pt - lastPingsTimedOut_) + " timed out (rtt " + std::to_string(lastRttMs_.load()) +
                " ms), " + std::to_string(sh - lastShortPacketsIn_) + " other short packets";
        lastPingsSent_ = ps;
        lastPingsEchoed_ = pe;
        lastPingsTimedOut_ = pt;
        lastShortPacketsIn_ = sh;
        VV_LOGI("conn", "audio packets in the last 10 s (%s): %s", context_.c_str(), line.c_str());
        return webrtc::TimeDelta::Seconds(10);
    });
}

void Connection::ConfigureSendStream()
{
    int payloadType = Get(audioEncoder_, "type", kAudioPayloadType);
    int channels = Get(audioEncoder_, "channels", context_ == "stream" ? 2 : 1);
    int bitrate = audioBitrateOverride_ > 0 ? audioBitrateOverride_ : voiceBitrate_;

    webrtc::AudioSendStream::Config config(transport_.get());
    config.rtp.ssrc = ssrc_;
    config.rtp.c_name = RandomCname();
    config.rtp.extensions = {webrtc::RtpExtension(webrtc::RtpExtension::kAudioLevelUri, webrtc::RtpHeaderExtensionId(1)),
                             webrtc::RtpExtension(webrtc::RtpExtension::kAbsSendTimeUri, webrtc::RtpHeaderExtensionId(3))};
    config.rtcp_report_interval_ms = 5000;
    config.min_bitrate_bps = 6000;
    config.max_bitrate_bps = bitrate;
    config.encoder_factory = webrtc::CreateBuiltinAudioEncoderFactory();
    webrtc::SdpAudioFormat format("opus", 48000, 2,
                                  {{"stereo", channels == 2 ? "1" : "0"},
                                   {"useinbandfec", fec_ ? "1" : "0"},
                                   {"usedtx", "0"},
                                   {"maxaveragebitrate", std::to_string(bitrate)}});
    config.send_codec_spec = webrtc::AudioSendStream::Config::SendCodecSpec(payloadType, format);
    config.send_codec_spec->target_bitrate_bps = bitrate;
    config.frame_encryptor = gate_;

    if (!sendStream_) {
        sendStream_ = call_->CreateAudioSendStream(config);
        sendStream_->Start();
    } else {
        sendStream_->Reconfigure(config, nullptr);
    }
    VV_LOGI("conn", "audio send stream: ssrc %u pt %d channels %d bitrate %d fec %d", ssrc_, payloadType, channels, bitrate, fec_);
}

void Connection::UpdateReceiveStream(RemoteUser& user)
{
    DestroyReceiveStream(user);
    if (!user.decryptor) user.decryptor = webrtc::make_ref_counted<FrameDecryptor>(dave_.get(), user.id);
    if (user.ssrc == 0 || !call_) return;

    webrtc::AudioReceiveStreamInterface::Config config;
    config.rtp.remote_ssrc = user.ssrc;
    config.rtcp_send_transport = transport_.get();
    config.decoder_factory = webrtc::CreateBuiltinAudioDecoderFactory();
    config.jitter_buffer_max_packets = 200;
    config.jitter_buffer_min_delay_ms = minimumJitterMs_;
    std::map<int, webrtc::SdpAudioFormat> decoders;
    if (audioDecoders_.is_array())
        for (auto& d : audioDecoders_) {
            if (Get<std::string>(d, "name", "") != "opus") continue;
            decoders.emplace(Get(d, "type", kAudioPayloadType), webrtc::SdpAudioFormat("opus", 48000, 2, {{"stereo", "1"}}));
        }
    if (decoders.empty()) decoders.emplace(kAudioPayloadType, webrtc::SdpAudioFormat("opus", 48000, 2, {{"stereo", "1"}}));
    config.decoder_map = std::move(decoders);
    config.frame_decryptor = user.decryptor;
    config.sync_group = user.id; // lip sync with the user's video

    user.stream = call_->CreateAudioReceiveStream(std::move(config));
    user.stream->Start();
    audioSsrcs_[user.ssrc] = user.id;
    ApplyGains(user);
    VV_LOGI("conn", "audio receive stream for %s ssrc %u", user.id.c_str(), user.ssrc);
}

void Connection::DestroyReceiveStream(RemoteUser& user)
{
    if (!user.stream) return;
    Engine::Get().PlayoutMixer().RemoveSourceGain(user.stream->remote_ssrc());
    audioSsrcs_.erase(user.stream->remote_ssrc());
    user.stream->Stop();
    call_->DestroyAudioReceiveStream(user.stream);
    user.stream = nullptr;
}

void Connection::UpdateVideoStreams(RemoteUser& user, bool recreate)
{
    if (!call_) return;
    std::set<uint32_t> wanted;
    if (!user.videoDisabled)
        for (auto v : user.videoSsrcs)
            if (v) wanted.insert(v);
    for (auto it = user.video.begin(); it != user.video.end();) {
        if (wanted.count(it->first) && (!recreate || it->second.decoders == videoDecoders_)) {
            ++it;
            continue;
        }
        it->second.watchdog.Stop();
        call_->DestroyVideoReceiveStream(it->second.stream);
        VV_LOGI("video", "video receive stream for %s ssrc %u destroyed", user.id.c_str(), it->first);
        it = user.video.erase(it);
    }

    std::vector<webrtc::VideoReceiveStreamInterface::Decoder> decoders;
    std::map<int, int> rtx;
    if (videoDecoders_.is_array())
        for (auto& d : videoDecoders_) {
            auto name = WebrtcCodecName(Get<std::string>(d, "name", ""));
            int type = Get(d, "type", 0);
            if (name.empty() || type <= 0) continue;
            webrtc::CodecParameterMap params;
            if (d.contains("params") && d["params"].is_object())
                for (auto& [k, v] : d["params"].items()) params[k] = v.is_string() ? v.get<std::string>() : v.dump();
            decoders.emplace_back(webrtc::SdpVideoFormat(name, params), type);
            if (int rtxType = Get(d, "rtxType", 0); rtxType > 0) rtx[rtxType] = type;
        }
    if (decoders.empty()) return; // no video codec negotiated yet

    if (!user.decryptor) user.decryptor = webrtc::make_ref_counted<FrameDecryptor>(dave_.get(), user.id);
    for (auto ssrc : wanted) {
        if (user.video.count(ssrc)) continue;
        VideoStream vs;
        vs.sink = std::make_unique<RemoteVideoSink>(this, user.id, ssrc);
        vs.decoders = videoDecoders_;
        webrtc::VideoReceiveStreamInterface::Config config(transport_.get(), decoderFactory_.get());
        config.rtp.remote_ssrc = ssrc;
        config.rtp.rtx_ssrc = ssrc + 1;
        config.rtp.rtx_associated_payload_types = rtx;
        config.rtp.nack.rtp_history_ms = 1000;
        config.rtp.keyframe_method = webrtc::KeyFrameReqMethod::kPliRtcp;
        config.decoders = decoders;
        config.renderer = vs.sink.get();
        config.frame_decryptor = user.decryptor;
        // libwebrtc only runs the frame decryptor of a video stream when frame encryption is required
        // (rtp_video_stream_receiver2.cc). FrameDecryptor passes frames through while DAVE is off
        config.crypto_options.sframe.require_frame_encryption = true;
        config.sync_group = user.id;
        config.on_first_packet = [this](uint32_t ssrc) { OnVideoSeen(ssrc); };
        vs.stream = call_->CreateVideoReceiveStream(std::move(config));
        vs.stream->Start();
        // A stream that missed the sender's key frame (created after it, or recreated by setDisableLocalVideo)
        // shows nothing until the next one, and libwebrtc does not ask for it while nothing has been decrypted
        // (require_frame_encryption). Ask every second until a frame is rendered
        vs.watchdog = webrtc::RepeatingTaskHandle::DelayedStart(
            Engine::Get().Worker(), webrtc::TimeDelta::Millis(300),
            [stream = vs.stream, sink = vs.sink.get(), id = user.id, ssrc, tick = 0]() mutable {
                if (sink->Rendered()) return webrtc::TimeDelta::PlusInfinity();
                auto s = stream->GetStats();
                if (tick++ % 5 == 0)
                    VV_LOGI("video", "waiting for a frame from %s ssrc %u: %u packets, pt %d, frames %d key / %d delta, %u decoded",
                            id.c_str(), ssrc, static_cast<unsigned>(s.rtp_stats.packet_counter.packets), s.current_payload_type,
                            s.received_frame_counts.key_frames, s.received_frame_counts.delta_frames, s.frames_decoded);
                stream->GenerateKeyFrame();
                return webrtc::TimeDelta::Seconds(1);
            });
        user.video.emplace(ssrc, std::move(vs));
        VV_LOGI("video", "video receive stream for %s ssrc %u (%zu decoders)", user.id.c_str(), ssrc, decoders.size());
    }
}

void Connection::DestroyVideoStreams(RemoteUser& user)
{
    for (auto& [ssrc, vs] : user.video) {
        vs.watchdog.Stop();
        call_->DestroyVideoReceiveStream(vs.stream);
    }
    user.video.clear();
}

// First RTP packet of a remote video stream (worker thread): JS learns the stream id to attach a sink to
void Connection::OnVideoSeen(uint32_t ssrc)
{
    std::string userId;
    {
        std::lock_guard lock(usersMutex_);
        auto it = videoSsrcs_.find(ssrc);
        if (it == videoSsrcs_.end() || !videoSeen_.insert(ssrc).second) return;
        userId = it->second;
    }
    VV_LOGI("video", "video from %s ssrc %u", userId.c_str(), ssrc);
    callbacks.video.Call([userId, ssrc](Napi::Env env) {
        return JsCallback::Args{Napi::String::New(env, userId), Napi::Number::New(env, ssrc),
                                Napi::String::New(env, std::to_string(ssrc)), env.Undefined()};
    });
}

// Worker, every 50 ms: a remote user speaks while their decoded audio is above about -40 dBFS, and 300 ms after
void Connection::UpdateRemoteSpeaking()
{
    constexpr int kAudibleLevel = 330;      // AudioReceiveStream's audio_level is 0..32767
    constexpr int64_t kHoldMs = 300;
    int64_t now = NowMs();
    std::vector<std::pair<std::string, int>> changes;
    {
        std::lock_guard lock(usersMutex_);
        for (auto& [id, user] : users_) {
            if (!user.stream) continue;
            if (user.stream->GetStats(false).audio_level > kAudibleLevel) user.lastAudibleMs = now;
            bool speaking = now - user.lastAudibleMs < kHoldMs;
            // the gateway's flags say what the audio is (voice, soundshare, priority); voice if it said nothing
            int flags = speaking ? (user.speakingFlags & ~kSpeakingHidden ? user.speakingFlags & ~kSpeakingHidden : kSpeakingVoice) : 0;
            if (flags == user.reportedFlags) continue;
            user.reportedFlags = flags;
            changes.emplace_back(id, flags);
        }
    }
    for (auto& [id, flags] : changes)
        callbacks.speaking.Call([id, flags](Napi::Env env) {
            return JsCallback::Args{Napi::String::New(env, id), Napi::Number::New(env, flags), env.Null()};
        });
}

void Connection::ApplyGains(RemoteUser& user)
{
    if (!user.stream) return;
    user.stream->SetGain(user.mute ? 0 : user.volume);
    Mixer::SourceGain g;
    g.left = user.panLeft;
    g.right = user.panRight;
    g.muted = user.mute || selfDeafen_;
    Engine::Get().PlayoutMixer().SetSourceGain(user.ssrc, g);
}

// ---------------------------------------------------------------------------------- options & users (JS)

void Connection::SetTransportOptions(const json& o)
{
    if (o.contains("selfMute")) SetSelfMute(Get(o, "selfMute", false));
    inputMode_ = Get(o, "inputMode", inputMode_.load());
    if (o.contains("inputModeOptions") && o["inputModeOptions"].is_object()) {
        auto& m = o["inputModeOptions"];
        vadThreshold_ = Get(m, "vadThreshold", vadThreshold_.load());
        vadAutoThreshold_ = Get(m, "vadAutoThreshold", vadAutoThreshold_.load());
        vadUseKrisp_ = Get(m, "vadUseKrisp", vadUseKrisp_.load());
        vadKrispThreshold_ = Get(m, "vadKrispActivationThreshold", vadKrispThreshold_.load());
        vadTrailing_ = Get(m, "vadTrailing", vadTrailing_.load());
        pttReleaseDelay_ = Get(m, "pttReleaseDelay", pttReleaseDelay_.load());
        Engine::Get().SetVadAggressiveness(vadAutoThreshold_);
    }
    if (o.contains("qos")) socket_.SetDscp(Get(o, "qos", false) ? 46 : 0);
    if (o.contains("soundsharePid") && context_ == "stream") {
        auto pid = static_cast<uint32_t>(std::max(0, Get(o, "soundsharePid", 0)));
        std::lock_guard lock(soundshareMutex_);
        if (pid || soundshare_) EnsureSoundshare()->SetPid(pid);
    }

    if (o.contains("encryptionSettings") && o["encryptionSettings"].is_object()) {
        auto& e = o["encryptionSettings"];
        auto mode = ParseCryptoMode(Get<std::string>(e, "mode", ""));
        Bytes key;
        if (e.contains("secretKey") && e["secretKey"].is_array())
            for (auto& b : e["secretKey"]) key.push_back(static_cast<uint8_t>(b.get<int>()));
        if (crypto_.Configure(mode, key)) {
            VV_LOGI("conn", "encryption: %s", CryptoModeName(mode));
            gate_->SendSilence();
        }
        else VV_LOGE("conn", "unsupported encryption settings (mode %s, key %zu bytes)", Get<std::string>(e, "mode", "").c_str(), key.size());
    }

    bool sendChanged = false, receiveChanged = false;
    if (o.contains("audioEncoder") && o["audioEncoder"].is_object()) {
        audioEncoder_ = o["audioEncoder"];
        if (Get(audioEncoder_, "rate", 0) > 0 && !o.contains("encodingVoiceBitRate")) voiceBitrate_ = Get(audioEncoder_, "rate", voiceBitrate_);
        sendChanged = true;
    }
    if (o.contains("encodingVoiceBitRate")) {
        int b = std::clamp(Get(o, "encodingVoiceBitRate", voiceBitrate_), 6000, 510000);
        sendChanged |= b != voiceBitrate_;
        voiceBitrate_ = b;
    }
    if (o.contains("vesktopAudioBitrate")) {
        int b = std::max(0, Get(o, "vesktopAudioBitrate", 0));
        if (b > 0) b = std::clamp(b, 6000, 510000);
        sendChanged |= b != audioBitrateOverride_;
        audioBitrateOverride_ = b;
        VV_LOGI("conn", "audio bitrate override: %d", b);
    }
    if (o.contains("fec")) {
        bool f = Get(o, "fec", fec_);
        sendChanged |= f != fec_;
        fec_ = f;
    }
    if (o.contains("audioDecoders")) {
        audioDecoders_ = o["audioDecoders"];
        receiveChanged = true;
    }
    bool videoChanged = false;
    if (o.contains("videoDecoders") && o["videoDecoders"] != videoDecoders_) {
        videoDecoders_ = o["videoDecoders"];
        VV_LOGI("video", "video decoders: %s", videoDecoders_.dump().c_str());
        videoChanged = true;
    }
    if (o.contains("minimumJitterBufferLevel")) {
        minimumJitterMs_ = std::max(0, Get(o, "minimumJitterBufferLevel", 0));
        receiveChanged = true;
    }
    if (sendChanged || receiveChanged || videoChanged) {
        Engine::Get().Worker()->PostTask([this, sendChanged, receiveChanged, videoChanged] {
            if (!call_) return;
            if (sendChanged) ConfigureSendStream();
            std::lock_guard lock(usersMutex_);
            for (auto& [id, user] : users_) {
                if (receiveChanged && user.stream) user.stream->SetBaseMinimumPlayoutDelayMs(minimumJitterMs_);
                if (videoChanged) UpdateVideoStreams(user, true);
            }
        });
    }
    SetVideoSendOptions(o);
}

void Connection::SetSelfMute(bool mute)
{
    if (selfMute_.exchange(mute) != mute) VV_LOGI("conn", "self mute %d", mute);
}

void Connection::SetSelfDeafen(bool deaf)
{
    selfDeafen_ = deaf;
    Engine::Get().Worker()->PostTask([this] {
        std::lock_guard lock(usersMutex_);
        for (auto& [id, user] : users_) ApplyGains(user);
    });
}

void Connection::MergeUsers(const json& users)
{
    if (!users.is_array()) return;
    for (auto& u : users) {
        auto id = Get<std::string>(u, "id", "");
        if (id.empty() || id == userId_) continue;
        uint32_t ssrc = Get<uint32_t>(u, "ssrc", 0);
        std::vector<uint32_t> videoSsrcs;
        if (u.contains("videoSsrcs") && u["videoSsrcs"].is_array())
            for (auto& v : u["videoSsrcs"])
                if (v.is_number()) videoSsrcs.push_back(v.get<uint32_t>());
        if (uint32_t vs = Get<uint32_t>(u, "videoSsrc", 0); vs && videoSsrcs.empty()) videoSsrcs.push_back(vs);
        float volume = Get(u, "volume", 1.0f);
        bool mute = Get(u, "mute", false);
        dave_->CreateUser(id);
        Engine::Get().Worker()->PostTask([this, id, ssrc, videoSsrcs, volume, mute] {
            std::lock_guard lock(usersMutex_);
            auto [it, created] = users_.try_emplace(id);
            auto& user = it->second;
            user.id = id;
            user.volume = volume;
            user.mute = mute;
            if (created || user.ssrc != ssrc) {
                user.ssrc = ssrc;
                UpdateReceiveStream(user);
            } else {
                ApplyGains(user);
            }
            if (user.videoSsrcs != videoSsrcs) {
                ForgetVideoSsrcs(user, videoSsrcs);
                user.videoSsrcs = videoSsrcs;
                for (auto v : videoSsrcs) videoSsrcs_[v] = id, videoSsrcs_[v + 1] = id;
                UpdateVideoStreams(user, false);
            }
        });
    }
}

void Connection::DestroyUser(const std::string& userId)
{
    dave_->DestroyUser(userId);
    Engine::Get().Worker()->PostTask([this, userId] {
        std::lock_guard lock(usersMutex_);
        auto it = users_.find(userId);
        if (it == users_.end()) return;
        DestroyReceiveStream(it->second);
        DestroyVideoStreams(it->second);
        ForgetVideoSsrcs(it->second, {});
        users_.erase(it);
        VV_LOGI("conn", "user %s removed", userId.c_str());
    });
}

std::set<std::string> Connection::UserIds() const
{
    std::lock_guard lock(usersMutex_);
    std::set<std::string> ids;
    for (auto& [id, u] : users_) ids.insert(id);
    return ids;
}

#define VV_WITH_USER(userId, body)                                                                                     \
    Engine::Get().Worker()->PostTask([=, this] {                                                                       \
        std::lock_guard lock(usersMutex_);                                                                             \
        auto it = users_.find(userId);                                                                                 \
        if (it == users_.end()) return;                                                                                \
        auto& user = it->second;                                                                                       \
        body;                                                                                                          \
        ApplyGains(user);                                                                                              \
    })

void Connection::SetLocalVolume(const std::string& userId, float volume)
{
    VV_WITH_USER(userId, user.volume = std::max(0.0f, volume));
}

void Connection::SetLocalMute(const std::string& userId, bool mute)
{
    VV_WITH_USER(userId, user.mute = mute);
}

void Connection::SetLocalPan(const std::string& userId, float left, float right)
{
    VV_WITH_USER(userId, (user.panLeft = std::clamp(left, 0.0f, 1.0f), user.panRight = std::clamp(right, 0.0f, 1.0f)));
}

void Connection::SetRemoteUserSpeakingStatus(const std::string& userId, int flags)
{
    VV_WITH_USER(userId, user.speakingFlags = flags);
}

void Connection::SetRemoteUserCanHavePriority(const std::string& userId, bool can)
{
    VV_WITH_USER(userId, user.canHavePriority = can);
}

void Connection::SetMinimumOutputDelay(int ms)
{
    Engine::Get().Worker()->PostTask([this, ms] {
        std::lock_guard lock(usersMutex_);
        for (auto& [id, user] : users_)
            if (user.stream) user.stream->SetBaseMinimumPlayoutDelayMs(std::max(0, ms));
    });
}

void Connection::SetDisableLocalVideo(const std::string& userId, bool disabled)
{
    Engine::Get().Worker()->PostTask([this, userId, disabled] {
        std::lock_guard lock(usersMutex_);
        auto it = users_.find(userId);
        if (it == users_.end() || it->second.videoDisabled == disabled) return;
        it->second.videoDisabled = disabled;
        VV_LOGI("video", "video of %s %s", userId.c_str(), disabled ? "disabled" : "enabled");
        UpdateVideoStreams(it->second, false);
    });
}

// Video ssrcs of `user` not in `keep` stop routing; JS hears that their streams ended (worker, usersMutex_ held)
void Connection::ForgetVideoSsrcs(RemoteUser& user, const std::vector<uint32_t>& keep)
{
    for (auto v : user.videoSsrcs) {
        if (std::find(keep.begin(), keep.end(), v) != keep.end()) continue;
        videoSsrcs_.erase(v);
        videoSsrcs_.erase(v + 1);
        if (!videoSeen_.erase(v)) continue;
        callbacks.video.Call([id = user.id](Napi::Env env) {
            return JsCallback::Args{Napi::String::New(env, id), Napi::Number::New(env, 0), Napi::String::New(env, ""), env.Undefined()};
        });
    }
}

void Connection::SetPTTActive(bool active, bool priority)
{
    if (pttActive_ && !active) pttReleasedMs_ = NowMs();
    pttActive_ = active;
    pttPriority_ = priority;
}

void Connection::SetUdpEndpoint(const std::string& address, int port)
{
    std::optional<SocketAddress> target;
    if (address.empty()) target = originalRemote_;
    else target = SocketAddress::Resolve(address, port);
    if (!target) {
        VV_LOGE("conn", "retarget to %s:%d failed", address.c_str(), port);
        callbacks.connectionFailed.Call([](Napi::Env env) { return JsCallback::Args{Napi::String::New(env, "resolve failed")}; });
        return;
    }
    std::lock_guard lock(addressMutex_);
    remote_ = *target;
    VV_LOGI("conn", "UDP endpoint -> %s:%d", remote_.Host().c_str(), remote_.Port());
}

void Connection::FastUdpReconnect()
{
    // NAT bindings are refreshed by sending right away
    nextPingMs_ = 0;
}

void Connection::ConfigureRetries(int, int, int) {}

// ---------------------------------------------------------------------------------- send gate (audio)

void Connection::OnCaptureFrame(const CaptureFrameInfo& info)
{
    if (context_ == "stream") return; // sends soundshare (OnSoundshareFrame), not the microphone
    int64_t now = info.timeMs;
    // automatic sensitivity replaces the level by 0 / -100 dB from the VAD
    // and uses a fixed -50 dB threshold; manual uses the level and vadThreshold
    float level = info.levelDb;
    float threshold = vadThreshold_;
    if (vadAutoThreshold_ >= 0) {
        bool speech = info.webrtcVad;
        if (vadUseKrisp_ && info.voiceProbability >= 0) speech = info.voiceProbability > vadKrispThreshold_;
        level = speech ? 0 : -100;
        threshold = -50;
    }
    if (level >= threshold) lastSpeechMs_ = now;
    bool voice = now - lastSpeechMs_ < static_cast<int64_t>(vadTrailing_) * 10;

    bool open;
    if (inputMode_ == 2) open = pttActive_ || now - pttReleasedMs_ < pttReleaseDelay_;
    else open = voice;
    bool sending = open && !selfMute_;
    gate_->SetOpen(sending);

    int flags = sending ? (kSpeakingVoice | (inputMode_ == 2 && pttPriority_ ? kSpeakingPriority : 0)) : 0;
    if (speakingFlags_.exchange(flags) != flags) {
        auto id = userId_;
        callbacks.speaking.Call([id, flags](Napi::Env env) {
            return JsCallback::Args{Napi::String::New(env, id), Napi::Number::New(env, flags), env.Null()};
        });
    }

    if (selfMute_ && voice) {
        if (!speakingWhileMutedReported_) {
            speakingWhileMutedReported_ = true;
            callbacks.speakingWhileMuted.Call();
        }
    } else if (!voice) {
        speakingWhileMutedReported_ = false;
    }
}

// --------------------------------------------------------------------------------------- soundshare

Soundshare* Connection::EnsureSoundshare()
{
    if (!soundshare_) {
        Soundshare::Events events;
        events.attached = [this] {
            VV_LOGI("soundshare", "attached");
            // JS raises the stream's Opus bitrate (handleSoundshare)
            callbacks.soundshare.Call([](Napi::Env env) { return JsCallback::Args{Napi::Boolean::New(env, true)}; });
        };
        events.ended = [this] { callbacks.soundshareEnded.Call(); };
        events.failed = [this](int code, const std::string& reason) {
            callbacks.soundshareFailed.Call([code, reason](Napi::Env env) {
                return JsCallback::Args{Napi::Number::New(env, code), Napi::String::New(env, reason), Napi::Boolean::New(env, false)};
            });
        };
        soundshare_ = std::make_unique<Soundshare>([this](const int16_t* samples, float peak) { OnSoundshareFrame(samples, peak); },
                                                   std::move(events));
    }
    return soundshare_.get();
}

// soundshare thread: 10 ms of 48 kHz stereo
void Connection::OnSoundshareFrame(const int16_t* samples, float peak)
{
    if (!streamAudioState_ || destroyed_) return;
    int64_t now = webrtc::TimeMillis();
    if (peak > 0.001f) lastSoundshareMs_ = now; // about -60 dBFS
    // like voice: frames flow while there is sound (and 500 ms after), speaking flag SOUNDSHARE
    bool sending = now - lastSoundshareMs_ < 500;
    gate_->SetOpen(sending);
    int flags = sending ? kSpeakingSoundshare : 0;
    if (speakingFlags_.exchange(flags) != flags) {
        auto id = userId_;
        callbacks.speaking.Call([id, flags](Napi::Env env) {
            return JsCallback::Args{Napi::String::New(env, id), Napi::Number::New(env, flags), env.Null()};
        });
    }
    uint32_t newLevel = 0;
    streamAudioState_->audio_transport()->RecordedDataIsAvailable(samples, 480, 2 * sizeof(int16_t), 2, 48000, 0, 0, 0, false, newLevel);
}

// ---------------------------------------------------------------------------------- network thread

bool Connection::SendRaw(const uint8_t* data, size_t length)
{
    SocketAddress to;
    {
        std::lock_guard lock(addressMutex_);
        to = remote_;
    }
    if (!socket_.SendTo(data, length, to)) return false;
    packetsSent_++;
    bytesSent_ += length;
    return true;
}

bool Connection::SendMedia(const uint8_t* data, size_t length, bool rtcp, bool video, size_t* wireSize)
{
    if (!crypto_.Active() || !discovered_) return false;
    size_t clear = rtcp ? TransportCrypto::kRtcpUnencryptedLength : crypto_.RtpUnencryptedLength(data, length);
    // audio RTP and the connection's RTCP share a nonce counter, video + RTX another
    uint32_t counter = (video ? videoNonce_ : audioNonce_).fetch_add(1);
    Bytes out;
    if (!crypto_.Encrypt(data, length, clear, counter, out)) return false;
    if (wireSize) *wireSize = out.size();
    return SendRaw(out.data(), out.size());
}

void Connection::SendDiscovery()
{
    uint8_t pkt[74] = {};
    WriteU16(pkt, 1);
    WriteU16(pkt + 2, 70);
    WriteU32(pkt + 4, ssrc_);
    // sent twice, about 1 ms apart
    SendRaw(pkt, sizeof(pkt));
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    SendRaw(pkt, sizeof(pkt));
    lastDiscoveryMs_ = NowMs();
}

void Connection::SendPing()
{
    uint8_t pkt[8] = {0x13, 0x37, 0xca, 0xfe};
    uint32_t seq = pingSeq_++;
    WriteU32LE(pkt + 4, seq);
    pendingPings_[seq] = NowMs();
    if (SendRaw(pkt, sizeof(pkt))) pingsSent_++;
}

void Connection::NetworkLoop()
{
    SendDiscovery();
    uint8_t buf[2048];
    while (running_) {
        pollfd pfd{socket_.Fd(), POLLIN, 0};
        poll(&pfd, 1, 10);
        int64_t now = NowMs();

        SocketAddress from;
        ssize_t n;
        while ((n = socket_.RecvFrom(buf, sizeof(buf), from)) > 0) {
            packetsReceived_++;
            bytesReceived_ += n;
            HandlePacket(buf, static_cast<size_t>(n), from);
        }

        if (!discovered_) {
            if (now - lastDiscoveryMs_ >= 1000) SendDiscovery();
            continue;
        }
        if (now >= nextPingMs_) {
            SendPing();
            nextPingMs_ = now + pingIntervalMs_;
        }
        for (auto it = pendingPings_.begin(); it != pendingPings_.end();) {
            if (now - it->second < 4000) {
                ++it;
                continue;
            }
            uint32_t seq = it->first;
            it = pendingPings_.erase(it);
            pingsTimedOut_++;
            std::string host;
            int port;
            {
                std::lock_guard lock(addressMutex_);
                host = remote_.Host();
                port = remote_.Port();
            }
            callbacks.pingTimeout.Call([host, port, seq](Napi::Env env) {
                return JsCallback::Args{Napi::String::New(env, host), Napi::Number::New(env, port), Napi::Number::New(env, seq),
                                        Napi::Number::New(env, 4000)};
            });
        }
    }
}

void Connection::HandlePacket(const uint8_t* data, size_t length, const SocketAddress& from)
{
    // IP discovery response
    if (length == 74 && ReadU16(data) == 2) {
        if (discovered_) return;
        char ip[65] = {};
        memcpy(ip, data + 8, 64);
        localAddress_ = ip;
        localPort_ = ReadU16(data + 72);
        discovered_ = true;
        nextPingMs_ = NowMs() + 100;
        int64_t connectTime = NowMs() - createdMs_;
        VV_LOGI("conn", "IP discovery: %s:%d (%lld ms)", localAddress_.c_str(), localPort_, static_cast<long long>(connectTime));
        if (onConnect_) {
            auto addr = localAddress_;
            int port = localPort_;
            onConnect_->Call([addr, port, connectTime](Napi::Env env) {
                auto info = Napi::Object::New(env);
                info.Set("protocol", "udp");
                info.Set("address", addr);
                info.Set("port", port);
                info.Set("connectTime", static_cast<double>(connectTime));
                info.Set("createConnectionTime", 0);
                return JsCallback::Args{Napi::String::New(env, ""), info};
            });
        }
        return;
    }

    // keepalive answer: Discord's voice servers answer 13 37 ca fe + sequence with 13 37 f0 0d + the same sequence
    // (an echo, 13 37 ca fe, is what the fake SFU sends)
    if (length == 8 && (ReadU32(data) == 0x1337f00d || ReadU32(data) == 0x1337cafe)) {
        uint32_t seq = ReadU32LE(data + 4);
        auto it = pendingPings_.find(seq);
        if (it == pendingPings_.end()) return;
        int rtt = static_cast<int>(NowMs() - it->second);
        pendingPings_.erase(it);
        pingsEchoed_++;
        lastRttMs_ = rtt;
        auto host = from.Host();
        int port = from.Port();
        callbacks.ping.Call([rtt, host, port, seq](Napi::Env env) {
            return JsCallback::Args{Napi::Number::New(env, rtt), Napi::String::New(env, host), Napi::Number::New(env, port),
                                    Napi::Number::New(env, seq)};
        });
        return;
    }

    if (length < 12 || (data[0] >> 6) != 2) {
        // neither discovery, nor a ping echo, nor RTP / RTCP: logged (a few) to see what the server sends instead
        shortPacketsIn_++;
        if (shortPacketsLogged_ < 5) {
            shortPacketsLogged_++;
            char hex[3 * 16 + 1] = {};
            for (size_t i = 0; i < std::min<size_t>(length, 16); i++) snprintf(hex + 3 * i, 4, "%02x ", data[i]);
            VV_LOGI("conn", "unexpected packet (%s), %zu bytes: %s", context_.c_str(), length, hex);
        }
        return;
    }
    if (IsRtcp(data, length)) {
        rtcpIn_++;
        HandleRtcp(data, length);
    } else {
        rtpIn_++;
        HandleRtp(data, length);
    }
}

void Connection::HandleRtp(const uint8_t* data, size_t length)
{
    if (!crypto_.Active()) return;
    Bytes plain;
    if (!crypto_.Decrypt(data, length, crypto_.RtpUnencryptedLength(data, length), plain)) {
        decryptionFailures_++;
        return;
    }
    uint32_t ssrc = ReadU32(plain.data() + 8);
    webrtc::MediaType type;
    {
        std::lock_guard lock(usersMutex_);
        if (audioSsrcs_.count(ssrc)) type = webrtc::MediaType::AUDIO;
        else if (videoSsrcs_.count(ssrc)) {
            type = webrtc::MediaType::VIDEO;
            if (videoPtLogged_.insert(ssrc).second)
                VV_LOGI("video", "first video packet on ssrc %u: payload type %d, %zu bytes", ssrc, plain[1] & 0x7f, plain.size());
        }
        else {
            routingFailures_++;
            return;
        }
    }
    webrtc::RtpPacketReceived packet(&extensions_, webrtc::Timestamp::Micros(NowUs()));
    if (!packet.Parse(webrtc::CopyOnWriteBuffer(plain.data(), plain.size()))) {
        routingFailures_++;
        return;
    }
    packet.set_payload_type_frequency(type == webrtc::MediaType::VIDEO ? 90000 : 48000);
    Engine::Get().Worker()->PostTask([this, type, packet = std::move(packet)]() mutable {
        if (!call_) return;
        call_->Receiver()->DeliverRtpPacket(type, std::move(packet), [this](const webrtc::RtpPacketReceived&) {
            routingFailures_++;
            return false;
        });
    });
}

// The SFU's REMB (RTCP PSFB FMT 15, "REMB"): the bitrate it lets us send, whatever our own estimate says.
// Logged when it changes (diagnostics for a stream that won't go above some bitrate)
void Connection::LogRemb(const uint8_t* p, size_t n)
{
    for (size_t off = 0; off + 4 <= n;) {
        size_t len = (static_cast<size_t>(ReadU16(p + off + 2)) + 1) * 4;
        if (off + len > n) break;
        const uint8_t* pk = p + off;
        if (pk[1] == 206 && (pk[0] & 0x1f) == 15 && len >= 20 && memcmp(pk + 12, "REMB", 4) == 0) {
            int exp = pk[17] >> 2;
            uint64_t mantissa = (static_cast<uint64_t>(pk[17] & 3) << 16) | (pk[18] << 8) | pk[19];
            uint64_t bps = mantissa << exp;
            if (bps != lastRemb_) {
                lastRemb_ = bps;
                VV_LOGI("conn", "SFU REMB (%s): %llu kbit/s", context_.c_str(), static_cast<unsigned long long>(bps / 1000));
            }
        }
        off += len;
    }
}

void Connection::HandleRtcp(const uint8_t* data, size_t length)
{
    if (!crypto_.Active()) return;
    Bytes plain;
    if (!crypto_.Decrypt(data, length, TransportCrypto::kRtcpUnencryptedLength, plain)) {
        decryptionFailures_++;
        return;
    }
    LogRemb(plain.data(), plain.size());
    webrtc::CopyOnWriteBuffer buffer(plain.data(), plain.size());
    Engine::Get().Worker()->PostTask([this, buffer = std::move(buffer)]() mutable {
        if (call_) call_->Receiver()->DeliverRtcpPacket(std::move(buffer));
    });
}

// ---------------------------------------------------------------------------------- local playback

void Connection::StartSamples(const std::string& id, const json& options, std::vector<std::vector<float>> channels,
                              JsCallbackPtr cb)
{
    int rate = Get(options, "sampleRate", 48000);
    float volume = Get(options, "volume", 1.0f);
    auto key = std::to_string(reinterpret_cast<uintptr_t>(this)) + ":" + id;
    Engine::Get().PlayoutMixer().StartSamples(key, rate, volume, std::move(channels), [cb] {
        if (cb) cb->Call([](Napi::Env env) { return JsCallback::Args{Napi::Number::New(env, 0), Napi::String::New(env, "")}; });
        Engine::Get().Worker()->PostTask([] { Engine::Get().UpdateAudioRunning(); });
    });
    Engine::Get().Worker()->PostTask([] { Engine::Get().UpdateAudioRunning(); });
}

void Connection::StopSamples(const std::string& id)
{
    Engine::Get().PlayoutMixer().StopSamples(std::to_string(reinterpret_cast<uintptr_t>(this)) + ":" + id);
}

void Connection::StopAllSamples()
{
    Engine::Get().PlayoutMixer().StopSamplesWithPrefix(std::to_string(reinterpret_cast<uintptr_t>(this)) + ":");
}

} // namespace vv
