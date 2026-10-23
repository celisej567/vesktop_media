// VoiceConnection: one UDP socket to the SFU, a webrtc::Call with our audio send
// stream and one audio receive stream per remote user, transport encryption and DAVE.
#pragma once

#include <api/scoped_refptr.h>
#include <call/video_receive_stream.h>
#include <rtc_base/task_utils/repeating_task.h>
#include <modules/rtp_rtcp/include/rtp_header_extension_map.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "common/js.h"
#include "dave/dave_session.h"
#include "engine/engine.h"
#include "transport/crypto.h"
#include "transport/udp_socket.h"

namespace webrtc {
class Call;
class AudioSendStream;
class AudioReceiveStreamInterface;
class VideoSendStream;
class Transport;
class VideoEncoderFactory;
class VideoBitrateAllocatorFactory;
} // namespace webrtc

namespace vv {

class FrameGate;
class Soundshare;
class FrameDecryptor;
class MediaTransport;
class RemoteVideoSink;
class DecoderFactory;
class FrameSource;
class PipeWireCapturer;
class VideoFrameEncryptor;

// Speaking flags (gateway SPEAKING)
enum SpeakingFlags { kSpeakingNone = 0, kSpeakingVoice = 1, kSpeakingSoundshare = 2, kSpeakingPriority = 4 };

class Connection {
public:
    Connection(std::string userId, const json& options, JsCallbackPtr onConnect);
    ~Connection();
    void Destroy();

    // JS API (JS thread)
    void SetTransportOptions(const json& options);
    void SetSelfMute(bool mute);
    void SetSelfDeafen(bool deaf);
    void MergeUsers(const json& users);
    void DestroyUser(const std::string& userId);
    void SetLocalVolume(const std::string& userId, float volume);
    void SetLocalMute(const std::string& userId, bool mute);
    void SetLocalPan(const std::string& userId, float left, float right);
    void SetRemoteUserSpeakingStatus(const std::string& userId, int flags);
    void SetRemoteUserCanHavePriority(const std::string& userId, bool can);
    void SetPTTActive(bool active, bool priority);
    void SetPingInterval(int ms) { pingIntervalMs_ = std::max(500, ms); }
    void SetUdpEndpoint(const std::string& address, int port);
    void FastUdpReconnect();
    void SetMinimumOutputDelay(int ms);
    void SetDisableLocalVideo(const std::string& userId, bool disabled);

    // Video send
    void SetVideoBroadcast(bool broadcast);
    void SetDesktopSource(const json& options); // setDesktopSourceWithOptions
    void ClearDesktopSource();
    // Go Live with a camera (+ microphone): {videoInputDeviceId, audioInputDeviceId}
    void SetGoLiveDevices(const json& options);
    void ClearGoLiveDevices();

    struct VideoSendOptions {
        int width = 0, height = 0, fps = 0;
        int bitrate = 0, minBitrate = 0, maxBitrate = 0;
        int degradation = 2;
        int sinkWantsPixels = 0, sinkWantsFps = 0;
        int keyframeIntervalMs = 0;
        bool alwaysSend = false;
        // Go Live: the encoder runs while the stream broadcasts. remoteSinkWantsPixelCount is 0 there for the
        // "Source" resolution (Discord's encode.pixelCount 0 = no limit), not "nobody watches"
        bool screen = false;
        // Vesktop extension (vesktopVideoBitrate): the stream's video bitrate chosen by the user, wins over
        // Discord's quality presets; 0 = Discord's
        int bitrateOverride = 0;
        bool smooth = false; // a screen encoded for frame rate over sharpness (set by UpdateVideoSend)
    };
    void ConfigureRetries(int baseDelay, int maxDelay, int maxAttempts);
    void GetStats(int filter, JsCallbackPtr cb);
    void StartSamples(const std::string& id, const json& options, std::vector<std::vector<float>> channels, JsCallbackPtr cb);
    void StopSamples(const std::string& id);
    void StopAllSamples();
    DaveSession& Dave() { return *dave_; }
    const std::string& UserId() const { return userId_; }
    std::set<std::string> UserIds() const;

    // Engine hooks
    void OnCaptureFrame(const CaptureFrameInfo& info); // audio thread
    bool Speaking() const { return speakingFlags_ > 0; }
    int VadAggressiveness() const { return vadAutoThreshold_; }

    struct Callbacks {
        CallbackSlot speaking, nativeMuteChanged, speakingWhileMuted, ping, pingTimeout, connectionFailed;
        CallbackSlot video, firstFrame, firstFrameDelivered, firstFrameEncrypted;
        CallbackSlot desktopSourceEnded, desktopSourceStatus, soundshare, soundshareEnded, soundshareFailed;
        CallbackSlot encoderFallback, decoderFallback, codecError, rtcpMessage;
        CallbackSlot mlsFailure, secureFramesState;
    } callbacks;

private:
    friend class FrameGate;
    friend class MediaTransport;
    friend class RemoteVideoSink;
    friend class VideoFrameEncryptor;

    struct VideoStream {
        webrtc::VideoReceiveStreamInterface* stream = nullptr;
        std::unique_ptr<RemoteVideoSink> sink;
        json decoders; // videoDecoders it was created with
        webrtc::RepeatingTaskHandle watchdog; // key frame requests until the first frame
    };

    struct RemoteUser {
        std::string id;
        uint32_t ssrc = 0;
        std::vector<uint32_t> videoSsrcs;
        float volume = 1;
        bool mute = false;
        float panLeft = 1, panRight = 1;
        int speakingFlags = 0;
        bool canHavePriority = false;
        webrtc::AudioReceiveStreamInterface* stream = nullptr;
        webrtc::scoped_refptr<FrameDecryptor> decryptor;
        bool videoDisabled = false;
        std::map<uint32_t, VideoStream> video; // video ssrc -> stream
    };

    // worker thread
    void CreateCall();
    void ConfigureSendStream();
    void UpdateReceiveStream(RemoteUser& user);
    void DestroyReceiveStream(RemoteUser& user);
    void ApplyGains(RemoteUser& user);
    void UpdateVideoStreams(RemoteUser& user, bool recreate);
    void DestroyVideoStreams(RemoteUser& user);
    void OnVideoSeen(uint32_t ssrc);
    void SetVideoSendOptions(const json& options); // JS thread, from setTransportOptions
    webrtc::Transport* RtpTransport();
    void UpdateVideoSend();        // worker
    void DestroyVideoSendStream(); // worker
    void ReportSelfVideo(uint32_t ssrc, const json& streams);
    json OutboundVideoStats();
    void ForgetVideoSsrcs(RemoteUser& user, const std::vector<uint32_t>& keep);
    json BuildStats(int filter);
    json VideoInboundStats(const std::string& userId, const webrtc::VideoReceiveStreamInterface::Stats& stats);

    // network thread
    void NetworkLoop();
    void HandlePacket(const uint8_t* data, size_t length, const SocketAddress& from);
    void HandleRtp(const uint8_t* data, size_t length);
    void HandleRtcp(const uint8_t* data, size_t length);
    void SendDiscovery();
    void SendPing();
    bool SendRaw(const uint8_t* data, size_t length);
    bool SendMedia(const uint8_t* data, size_t length, bool rtcp, bool video, size_t* wireSize = nullptr);

    const std::string userId_;
    const uint32_t ssrc_;
    std::string context_;
    std::string streamUserId_;
    std::string signingKeyId_;
    json streamParameters_;
    JsCallbackPtr onConnect_;
    std::atomic<bool> destroyed_{false};

    // network
    UdpSocket socket_;
    std::mutex addressMutex_;
    SocketAddress remote_;
    SocketAddress originalRemote_;
    std::thread networkThread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> discovered_{false};
    int64_t createdMs_ = 0;
    int64_t lastDiscoveryMs_ = 0;
    std::string localAddress_;
    int localPort_ = 0;
    std::atomic<int> pingIntervalMs_{5000};
    int64_t nextPingMs_ = 0;
    uint32_t pingSeq_ = 1;
    std::map<uint32_t, int64_t> pendingPings_;
    std::atomic<int> lastRttMs_{0};
    TransportCrypto crypto_;
    std::atomic<uint32_t> audioNonce_{0x80000000u};
    std::atomic<uint32_t> videoNonce_{0x80000000u};
    webrtc::RtpHeaderExtensionMap extensions_;

    // counters
    std::atomic<uint64_t> packetsSent_{0}, bytesSent_{0}, packetsReceived_{0}, bytesReceived_{0};
    std::atomic<uint64_t> decryptionFailures_{0}, routingFailures_{0};

    // webrtc (worker thread)
    std::unique_ptr<MediaTransport> transport_;
    std::unique_ptr<webrtc::Call> call_;
    webrtc::AudioSendStream* sendStream_ = nullptr;
    // stream connections: their own AudioState (shared playout mixer, no microphone) fed by soundshare
    webrtc::scoped_refptr<webrtc::AudioState> streamAudioState_;
    std::unique_ptr<Soundshare> soundshare_;
    std::mutex soundshareMutex_;
    int64_t lastSoundshareMs_ = -1'000'000; // soundshare thread
    Soundshare* EnsureSoundshare(); // soundshareMutex_ held
    void OnSoundshareFrame(const int16_t* samples, float peak);
    webrtc::scoped_refptr<FrameGate> gate_;
    json audioEncoder_;
    json audioDecoders_;
    int voiceBitrate_ = 64000;
    int audioBitrateOverride_ = 0; // Vesktop extension (vesktopAudioBitrate), wins over encodingVoiceBitRate
    bool fec_ = true;
    int minimumJitterMs_ = 0;
    json videoDecoders_;
    std::unique_ptr<DecoderFactory> decoderFactory_;

    // video send (worker thread)
    VideoSendOptions videoSendOptions_;
    json videoEncoder_;
    bool videoBroadcast_ = false;
    webrtc::VideoSendStream* videoSendStream_ = nullptr;
    webrtc::RepeatingTaskHandle videoStatsTask_; // logs the send stream's state every 5 s
    webrtc::RepeatingTaskHandle audioStatsTask_; // logs audio packets sent / received every 10 s (diagnostics)
    std::map<std::string, uint32_t> lastAudioReceived_;
    uint32_t lastAudioSent_ = 0;
    std::atomic<uint64_t> rtpIn_{0}, rtcpIn_{0}; // network thread: RTP / RTCP packets that arrived
    void LogRemb(const uint8_t* rtcp, size_t length);
    uint64_t lastRemb_ = 0;
    uint64_t lastRtpIn_ = 0, lastRtcpIn_ = 0, lastDecryptionFailures_ = 0, lastRoutingFailures_ = 0;
    json videoSendKey_;    // what the stream was created with (ssrcs, payload types)
    json videoEncoderKey_; // what the encoder was configured with
    webrtc::scoped_refptr<FrameSource> videoSendSource_;
    bool cameraAcquired_ = false;
    std::string goLiveVideoGuid_;   // worker: the camera streamed with Go Live ("" = none)
    std::string goLivePreviewGuid_; // worker: the camera whose frames also go to the stream's preview
    std::unique_ptr<webrtc::VideoEncoderFactory> encoderFactory_;
    std::unique_ptr<webrtc::VideoBitrateAllocatorFactory> bitrateAllocatorFactory_;
    webrtc::scoped_refptr<VideoFrameEncryptor> videoEncryptor_;
    std::set<std::string> encoderFallbacksReported_;
    json lastSelfVideo_;
    std::string videoCname_;
    // desktop source (JS thread + worker, under desktopMutex_)
    std::mutex desktopMutex_;
    webrtc::scoped_refptr<FrameSource> desktopSource_;
    std::unique_ptr<PipeWireCapturer> desktopCapturer_;

    mutable std::mutex usersMutex_;
    std::map<std::string, RemoteUser> users_;
    std::map<uint32_t, std::string> audioSsrcs_; // ssrc -> user
    std::map<uint32_t, std::string> videoSsrcs_; // video + rtx ssrc -> user
    std::set<uint32_t> videoSeen_;               // video ssrcs reported through the video callback
    std::set<uint32_t> videoPtLogged_;

    // send gate (audio thread)
    std::atomic<bool> selfMute_{false};
    std::atomic<bool> selfDeafen_{false};
    std::atomic<int> inputMode_{1}; // 1 voice activity, 2 push to talk
    std::atomic<float> vadThreshold_{-40};
    std::atomic<int> vadAutoThreshold_{-1};
    std::atomic<bool> vadUseKrisp_{false};
    std::atomic<float> vadKrispThreshold_{0.5f};
    std::atomic<int> vadTrailing_{25};
    std::atomic<int> pttReleaseDelay_{20};
    std::atomic<bool> pttActive_{false};
    std::atomic<bool> pttPriority_{false};
    std::atomic<int64_t> pttReleasedMs_{0};
    int64_t lastSpeechMs_ = -1'000'000;
    std::atomic<int> speakingFlags_{-1}; // -1: initial state not reported yet
    bool speakingWhileMutedReported_ = false;

    std::unique_ptr<DaveSession> dave_;
};

} // namespace vv
