// The media engine: one per process. Owns the WebRTC environment, the "Voice"
// worker thread every Call runs on, the audio device, audio processing and the playout mixer, and the
// engine level JS callbacks.
#pragma once

#include <api/environment/environment.h>
#include <api/scoped_refptr.h>

#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "audio/mixer.h"
#include "audio/volume_analyzer.h"
#include "common/js.h"

namespace webrtc {
class AudioProcessing;
class AudioState;
class Thread;
class AudioDeviceModule;
} // namespace webrtc

namespace vv {

class CaptureAdm;
class Connection;

// Per frame result of the send side analysis, shared by all connections
struct CaptureFrameInfo {
    int64_t timeMs = 0;
    float levelDb = -100;         // VolumeAnalyzer level of the processed frame
    bool webrtcVad = false;       // WebRTC GMM VAD with the requested aggressiveness
    float voiceProbability = -1;  // noise canceller (RNNoise) speech probability, -1 = off
};

struct ProcessingOptions {
    bool echoCancellation = true;
    bool noiseSuppression = true;
    bool automaticGainControl = true;
    bool noiseCancellation = false;
    void Update(const json& j);
    bool operator==(const ProcessingOptions&) const = default;
};

class Engine {
public:
    static Engine& Get();

    void Initialize(const json& options);
    bool Initialized() const { return initialized_; }

    webrtc::Thread* Worker() const { return worker_.get(); }
    const webrtc::Environment& Env() const { return *env_; }
    webrtc::scoped_refptr<webrtc::AudioState> AudioState() const;
    Mixer& PlayoutMixer() { return *mixer_; }
    const std::string& DataDirectory() const { return dataDirectory_; }
    bool FakeAudio() const { return fakeAudio_; }

    void AddConnection(Connection* connection);
    void RemoveConnection(Connection* connection);
    // Starts / stops capture and playout depending on what needs them. Worker thread
    void UpdateAudioRunning();

    // Engine level options
    void SetTransportOptions(const json& options);
    void SetVadAggressiveness(int mode); // from the connections' vadAutoThreshold

    // Devices
    json InputDevices();
    json OutputDevices();
    void SetInputDevice(const std::string& id);
    void SetOutputDevice(const std::string& id);
    void SetInputVolume(float linear);
    void SetOutputVolume(float linear);

    // Level meter / mic test / recording
    void SetEmitVadLevel(bool enabled);
    void SetNoInputThreshold(float db) { noInputThresholdDb_ = db; }
    void SetLoopback(bool enabled, const json& options);
    bool StartLocalRecording(const json& options);
    // returns the file name and duration
    std::pair<std::string, int64_t> StopLocalRecording();

    // Called from the APM capture post-processor (audio thread) with the processed mono frame
    void OnProcessedCapture(const float* samples, size_t frames);

    struct Callbacks {
        CallbackSlot onVoice;            // (levelDb, flags)
        CallbackSlot deviceChange;       // (inputs, outputs, videoInputs)
        CallbackSlot volumeChange;       // (input, output)
        CallbackSlot noInput;            // (hasInput)
        CallbackSlot admError;           // (code, message)
        CallbackSlot voiceProcessingError; // (code)
        CallbackSlot audioInputInit;     // (info)
        CallbackSlot videoInputInit;     // (info)
        CallbackSlot deviceHardwareMuted;
        CallbackSlot spatialAudioStatus;
        CallbackSlot videoFilterError;
        CallbackSlot videoCodecError;
        CallbackSlot pickerUpdate, pickerCancel, pickerError; // setNativeScreenSharePickerCallbacks
    } callbacks;

    struct Stats {
        uint64_t framesCaptured = 0;
        uint64_t noiseCancellerFrames = 0;
        uint64_t noiseCancellerTimeUs = 0;
        int64_t firstCaptureMs = 0;
        int64_t firstPlayoutMs = 0;
    };
    Stats GetStats() const;
    int64_t StartTimeMs() const { return startMs_; }

private:
    Engine() = default;
    void ApplyProcessing();
    void PollDevices();
    json DeviceList(bool input);
    int FindDevice(bool input, const std::string& id);
    void WriteRecording(const float* samples, size_t frames);

    bool initialized_ = false;
    std::string dataDirectory_;
    bool fakeAudio_ = false;
    int64_t startMs_ = 0;

    std::unique_ptr<webrtc::Environment> env_;
    std::unique_ptr<webrtc::Thread> worker_;
    webrtc::scoped_refptr<CaptureAdm> adm_;
    webrtc::scoped_refptr<webrtc::AudioProcessing> apm_;
    webrtc::scoped_refptr<Mixer> mixer_;
    webrtc::scoped_refptr<webrtc::AudioState> audioState_;

    mutable std::mutex mutex_;            // options
    mutable std::mutex connectionsMutex_; // connections_, held while frames are dispatched
    std::vector<Connection*> connections_;
    ProcessingOptions options_;
    std::optional<ProcessingOptions> loopbackOptions_;
    std::optional<ProcessingOptions> recordingOptions_;
    int recordingDevice_ = 0, playoutDevice_ = 0;
    // last setInput/OutputDevice id (worker thread)
    std::string recordingDeviceId_, playoutDeviceId_;
    bool recordingDeviceSet_ = false, playoutDeviceSet_ = false;
    json lastInputs_, lastOutputs_, lastVideoInputs_;

    // analysis (audio thread)
    VolumeAnalyzer analyzer_;
    void* vad_ = nullptr;
    std::atomic<int> vadMode_{3};
    int appliedVadMode_ = -1;
    std::atomic<bool> emitVadLevel_{false};
    int64_t lastVoiceEmitMs_ = 0;
    std::atomic<float> noInputThresholdDb_{-100};
    int64_t lastInputMs_ = 0;
    int noInputState_ = -1; // -1 unknown, 0 no input, 1 input
    std::atomic<uint64_t> framesCaptured_{0};
    std::atomic<int64_t> firstCaptureMs_{0};

    // voice message recording
    std::mutex recordingMutex_;
    FILE* recordingFile_ = nullptr;
    std::string recordingPath_;
    uint64_t recordingFrames_ = 0;
};

} // namespace vv
