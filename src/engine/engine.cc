#include "engine/engine.h"

#include <api/audio/audio_processing.h>
#include <api/audio/builtin_audio_processing_builder.h>
#include <api/audio/create_audio_device_module.h>
#include <api/environment/environment_factory.h>
#include <api/field_trials.h>
#include <api/make_ref_counted.h>
#include <api/units/time_delta.h>
#include <audio/audio_transport_impl.h>
#include <call/audio_sender.h>
#include <call/audio_state.h>
#include <modules/audio_processing/audio_buffer.h>
#include <rtc_base/logging.h>
#include <rtc_base/thread.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

#include "audio/adm.h"
#include "audio/deepfilter.h"
#include "audio/rnnoise.h"
#include "capture/camera.h"
#include "video/video_sinks.h"
#include "common/log.h"
#include "common/platform.h"
#include "common/util.h"
#include "connection/connection.h"

extern "C" {
struct WebRtcVadInst;
WebRtcVadInst* WebRtcVad_Create();
int WebRtcVad_Init(WebRtcVadInst* handle);
int WebRtcVad_set_mode(WebRtcVadInst* handle, int mode);
int WebRtcVad_Process(WebRtcVadInst* handle, int fs, const int16_t* audio_frame, size_t frame_length);
}

namespace vv {

namespace {

// How long the mic test is silenced after the processing changed, in 10 ms frames
constexpr int kSettleFrames = 35;

// Field trials Discord's clients run with
constexpr char kFieldTrials[] =
    "WebRTC-Audio-NetEqDecisionLogicConfig/reinit_after_expands:100/"
    "WebRTC-Bwe-RobustThroughputEstimatorSettings/enabled:true/"
    // Discord's "loss-based-bwe-v2" experiment: losses reported by the SFU's transport feedback no longer
    // collapse the estimate to the floor (v1 halves it on every lossy report)
    "WebRTC-Bwe-LossBasedBweV2/Enabled:true/"
    "WebRTC-Video-H26xPacketBuffer/Enabled/";

// Takes the audio libwebrtc would encode and drops it. See Engine::UpdateAudioRunning
class NullAudioSender : public webrtc::AudioSender {
public:
    void SendAudioData(std::unique_ptr<webrtc::AudioFrame>) override {}
};

// Hands the processed capture to the engine (the module's VadObserver::OnPreEncode)
class CaptureTap : public webrtc::CustomProcessing {
public:
    void Initialize(int sampleRateHz, int channels) override
    {
        // the rate libwebrtc captures at: 48 kHz with a sending connection, lower (8 kHz) without one
        VV_LOGI("engine", "capture processing at %d Hz, %d channel(s)", sampleRateHz, channels);
        Engine::Get().SetCaptureRate(sampleRateHz);
    }
    void Process(webrtc::AudioBuffer* audio) override
    {
        if (audio->num_channels() == 0) return;
        Engine::Get().OnProcessedCapture(audio->channels_const()[0], audio->num_frames());
    }
    std::string ToString() const override { return "vesktop_media capture tap"; }
};

} // namespace

void ProcessingOptions::Update(const json& j)
{
    if (!j.is_object()) return;
    echoCancellation = Get(j, "echoCancellation", echoCancellation);
    noiseSuppression = Get(j, "noiseSuppression", noiseSuppression);
    noiseCancellation = Get(j, "noiseCancellation", noiseCancellation);
    automaticGainControl = Get(j, "automaticGainControl", automaticGainControl);
    if (j.contains("automaticGainControlConfig") && j["automaticGainControlConfig"].is_object())
        automaticGainControl = Get(j["automaticGainControlConfig"], "enabled", automaticGainControl);
}

Engine& Engine::Get()
{
    static Engine* engine = new Engine(); // never destroyed: WebRTC threads may outlive static destructors
    return *engine;
}

namespace {

// libwebrtc's own log (warnings and errors; everything with logLevel >= 3) into vesktop_media.log
class WebrtcLogSink : public webrtc::LogSink {
public:
    void OnLogMessage(const std::string& message) override { OnLogMessage(message, webrtc::LS_INFO); }
    void OnLogMessage(const std::string& message, webrtc::LoggingSeverity severity) override
    {
        LogLevel level = severity >= webrtc::LS_ERROR ? LogLevel::Error
                         : severity == webrtc::LS_WARNING ? LogLevel::Warning
                         : severity == webrtc::LS_INFO ? LogLevel::Info : LogLevel::Verbose;
        std::string m = message;
        while (!m.empty() && (m.back() == '\n' || m.back() == '\r')) m.pop_back();
        Log(level, "webrtc", "%s", m.c_str());
    }
};

} // namespace

void Engine::Initialize(const json& options)
{
    if (initialized_) return;
    startMs_ = NowMs();
    auto dir = vv::Get<std::string>(options, "dataDirectory", "");
    auto logDir = vv::Get<std::string>(options, "logDirectory", dir);
    int logLevel = vv::Get(options, "logLevel", -1);
    LogInit(logDir, logLevel, vv::Get<size_t>(options, "maxLogBytes", 5'000'000));
    static WebrtcLogSink webrtcLog;
    webrtc::LogMessage::LogToDebug(webrtc::LS_NONE); // no stderr
    webrtc::LogMessage::SetLogToStderr(false);
    webrtc::LogMessage::AddLogToStream(&webrtcLog, logLevel >= 3 ? webrtc::LS_INFO : webrtc::LS_WARNING);
    dataDirectory_ = dir.empty() ? PathToUtf8(std::filesystem::temp_directory_path()) : dir;
    fakeAudio_ = vv::Get(options, "useFakeAudioCapture", false);
    Camera::Get().SetFake(vv::Get(options, "useFakeVideoCapture", false));
    Camera::Get().SetSelectionChangedCallback([this] {
        std::lock_guard lock(connectionsMutex_);
        for (auto* c : connections_) c->OnVideoDeviceSelected();
    });
    // a JS preview sink on the camera's guid starts the camera
    VideoSinks::Get().SetObserver([](const std::string& streamId, bool attached) { Camera::Get().OnPreviewSinkChanged(streamId, attached); });
    Camera::Get().Refresh(); // first enumeration, on the camera thread
    VV_LOGI("engine", "initialize: %s", options.dump().c_str());

    env_ = std::make_unique<webrtc::Environment>(webrtc::CreateEnvironment(webrtc::FieldTrials::Create(kFieldTrials)));
    worker_ = webrtc::Thread::Create();
    worker_->SetName("Voice", nullptr);
    worker_->Start();

    vad_ = WebRtcVad_Create();
    WebRtcVad_Init(static_cast<WebRtcVadInst*>(vad_));
    mixer_ = webrtc::make_ref_counted<Mixer>();

    worker_->BlockingCall([&] {
        // PipeWire's pulse server copies a client's properties to its streams. With node.dont-move the microphone
        // stream stays on the source Discord picked: tools like Easy Effects move every ordinary recording stream
        // onto their own source, so the microphone and "Easy Effects Source" would both end up on the latter.
        // libwebrtc's PulseAudio module cannot set stream properties (or the DONT_MOVE flag, which does not stop that
        // move), so the client's own are set while it connects, and given back right after
#ifndef _WIN32
        const char* oldProp = getenv("PULSE_PROP");
        std::string oldPropValue = oldProp ? oldProp : "";
        setenv("PULSE_PROP", (oldPropValue.empty() ? std::string() : oldPropValue + " ") .append("node.dont-move=true").c_str(), 1);
#endif
        webrtc::scoped_refptr<webrtc::AudioDeviceModule> platform;
        if (!fakeAudio_) platform = webrtc::CreateAudioDeviceModule(*env_, webrtc::AudioDeviceModule::kPlatformDefaultAudio);
        if (!platform) {
            if (!fakeAudio_) VV_LOGE("engine", "no audio device module, using fake audio");
            fakeAudio_ = true;
            platform = CreateFakeAdm();
        }
        adm_ = CaptureAdm::Create(platform);

        apm_ = webrtc::BuiltinAudioProcessingBuilder().SetCapturePostProcessing(std::make_unique<CaptureTap>()).Build(*env_);

        webrtc::AudioState::Config config;
        config.audio_mixer = mixer_;
        config.audio_processing = apm_;
        config.audio_device_module = adm_;
        audioState_ = webrtc::AudioState::Create(config);

        adm_->Init();
#ifndef _WIN32
        if (oldProp) setenv("PULSE_PROP", oldPropValue.c_str(), 1);
        else unsetenv("PULSE_PROP");
#endif
        adm_->RegisterAudioCallback(audioState_->audio_transport());
        SelectDevice(false, kDefaultDevice);
        SelectDevice(true, kDefaultDevice);
        adm_->InitSpeaker();
        adm_->InitMicrophone();
        bool stereo = false;
        adm_->StereoPlayoutIsAvailable(&stereo);
        adm_->SetStereoPlayout(stereo);
        adm_->SetStereoRecording(false);
        webrtc::AudioDeviceModule::AudioLayer layer;
        adm_->ActiveAudioLayer(&layer);
        VV_LOGI("engine", "audio device module ready, layer %d, %d inputs, %d outputs", static_cast<int>(layer),
                adm_->RecordingDevices(), adm_->PlayoutDevices());
        lastInputs_ = DeviceList(true);
        lastOutputs_ = DeviceList(false);
        ApplyProcessing();
    });
    initialized_ = true;
    worker_->PostDelayedTask([this] { PollDevices(); }, webrtc::TimeDelta::Seconds(2));
}

webrtc::scoped_refptr<webrtc::AudioState> Engine::AudioState() const
{
    return audioState_;
}

void Engine::AddConnection(Connection* connection)
{
    {
        std::lock_guard lock(connectionsMutex_);
        connections_.push_back(connection);
    }
    worker_->PostTask([this] { UpdateAudioRunning(); });
}

void Engine::RemoveConnection(Connection* connection)
{
    {
        std::lock_guard lock(connectionsMutex_);
        connections_.erase(std::remove(connections_.begin(), connections_.end(), connection), connections_.end());
        mixer_->StopSamplesWithPrefix(std::to_string(reinterpret_cast<uintptr_t>(connection)) + ":");
    }
    worker_->PostTask([this] { UpdateAudioRunning(); });
}

void Engine::UpdateAudioRunning()
{
    bool hasConnections, recording;
    {
        std::lock_guard lock(connectionsMutex_);
        hasConnections = !connections_.empty();
    }
    ApplyProcessing();
    {
        std::lock_guard lock(recordingMutex_);
        recording = recordingFile_ != nullptr;
    }
    bool loopback = mixer_->LoopbackEnabled();
    bool needCapture = hasConnections || emitVadLevel_ || loopback || recording;
    bool needPlayout = hasConnections || loopback || mixer_->HasSamples();

    // Without a sender libwebrtc captures at its default 8 kHz (the APM runs at 16 kHz): the signal is band limited
    // before any processing, and the mic test and voice messages sounded like a phone. A sender that discards
    // everything makes it capture at 48 kHz. Only while no connection exists: a connection's own send stream sets
    // the rate (and the senders) itself and would be replaced by this
    if (needCapture && !hasConnections && audioState_) {
        static NullAudioSender nullSender;
        static_cast<webrtc::AudioTransportImpl*>(audioState_->audio_transport())->UpdateAudioSenders({&nullSender}, 48000, 1);
    }
    if (needCapture && !adm_->Recording()) {
        if (adm_->InitRecording() == 0 && adm_->StartRecording() == 0) VV_LOGI("engine", "capture started");
        else callbacks.admError.Call([](Napi::Env env) {
            return JsCallback::Args{Napi::Number::New(env, -1), Napi::String::New(env, "failed to start recording")};
        });
    } else if (!needCapture && adm_->Recording()) {
        adm_->StopRecording();
        analyzer_.Reset();
        noInputState_ = -1;
        VV_LOGI("engine", "capture stopped");
    }
    if (needPlayout && !adm_->Playing()) {
        if (adm_->InitPlayout() == 0 && adm_->StartPlayout() == 0) VV_LOGI("engine", "playout started");
    } else if (!needPlayout && adm_->Playing()) {
        adm_->StopPlayout();
        VV_LOGI("engine", "playout stopped");
    }
}

void Engine::ApplyProcessing()
{
    if (!apm_) return;
    bool noConnections;
    {
        std::lock_guard lock(connectionsMutex_);
        noConnections = connections_.empty();
    }
    // the mic test / voice message options apply while nothing else uses the mic
    ProcessingOptions o;
    {
        std::lock_guard lock(mutex_);
        o = options_;
        if (noConnections && loopbackOptions_) o = *loopbackOptions_;
        else if (noConnections && recordingOptions_) o = *recordingOptions_;
    }

    // Discord's noise cancellation flag runs the selected model; Standard, or a model that cannot run here (no
    // library, a DeepFilterNet state that could not be created), is libwebrtc's suppression
    NoiseModel model = noiseModel_;
    auto denoiser = CaptureAdm::Denoiser::None;
    if (o.noiseCancellation) {
        if (model == NoiseModel::RnNoise) denoiser = CaptureAdm::Denoiser::RnNoise;
        else if (model == NoiseModel::DeepFilter && NoiseModelSupported(NoiseModel::DeepFilter)) denoiser = CaptureAdm::Denoiser::DeepFilter;
        else {
            o.noiseCancellation = false;
            o.noiseSuppression = true;
        }
    }

    // Unchanged options are not applied again: Discord repeats them with every setting, and a configuration set
    // again can reset the processing stages. The model counts too: RNNoise and DeepFilterNet are the same flags
    if (appliedOptions_ && *appliedOptions_ == o && appliedDenoiser_ == static_cast<int>(denoiser)) return;
    bool changed = appliedOptions_.has_value();
    appliedOptions_ = o;
    appliedDenoiser_ = static_cast<int>(denoiser);
    // A stage that was just switched on starts from nothing (noise estimate, echo filter, RNNoise state) and lets
    // noise through for a moment: the mic test stays silent meanwhile
    if (changed && mixer_->LoopbackEnabled()) mixer_->DuckLoopback(kSettleFrames);

    webrtc::AudioProcessing::Config c;
    c.pipeline.maximum_internal_processing_rate = 48000;
    c.high_pass_filter.enabled = true;
    c.echo_canceller.enabled = o.echoCancellation;
    c.noise_suppression.enabled = o.noiseSuppression;
    c.noise_suppression.level = webrtc::AudioProcessing::Config::NoiseSuppression::kHigh;
    // AGC2 adaptive digital with Discord's parameters. The analog AGC1 stage is left out: it would move the
    // system mic volume
    c.gain_controller1.enabled = false;
    c.gain_controller2.enabled = o.automaticGainControl;
    c.gain_controller2.adaptive_digital.enabled = o.automaticGainControl;
    c.gain_controller2.adaptive_digital.headroom_db = 5;
    c.gain_controller2.adaptive_digital.max_gain_db = 50;
    c.gain_controller2.adaptive_digital.initial_gain_db = 15;
    c.gain_controller2.adaptive_digital.max_gain_change_db_per_second = 6;
    c.gain_controller2.adaptive_digital.max_output_noise_level_dbfs = -50;
    apm_->ApplyConfig(c);

    if (denoiser == CaptureAdm::Denoiser::RnNoise && !adm_->NoiseCancellationAvailable())
        callbacks.voiceProcessingError.Call([](Napi::Env env) { return JsCallback::Args{Napi::Number::New(env, 2)}; });
    adm_->SetDenoiser(denoiser);
    static const char* const kDenoisers[] = {"none", "RNNoise", "DeepFilterNet"};
    VV_LOGI("engine", "processing applied: aec=%d ns=%d agc=%d nc=%d (model %d, denoiser %s)", o.echoCancellation,
            o.noiseSuppression, o.automaticGainControl, o.noiseCancellation, static_cast<int>(model),
            kDenoisers[static_cast<int>(denoiser)]);
}

void Engine::OnDenoiserFailed()
{
    // the noise cancellation flag falls back to the standard suppression (NoiseModelSupported is false now)
    if (initialized_) worker_->PostTask([this] { ApplyProcessing(); });
}

void Engine::SetNoiseModel(NoiseModel model)
{
    if (noiseModel_.exchange(model) == model) return;
    VV_LOGI("engine", "noise model: %d", static_cast<int>(model));
    if (initialized_) worker_->PostTask([this] { ApplyProcessing(); });
}

bool Engine::NoiseModelSupported(NoiseModel model)
{
    switch (model) {
    case NoiseModel::Standard: return true;
    case NoiseModel::RnNoise: return RnNoise::Available();
    case NoiseModel::DeepFilter: return DeepFilter::Available() && !DeepFilter::Failed();
    }
    return false;
}

void Engine::SetTransportOptions(const json& options)
{
    {
        std::lock_guard lock(mutex_);
        auto before = options_;
        options_.Update(options);
        if (before == options_) return;
        VV_LOGI("engine", "processing: aec=%d ns=%d agc=%d nc=%d", options_.echoCancellation, options_.noiseSuppression,
                options_.automaticGainControl, options_.noiseCancellation);
    }
    if (!initialized_) return;
    worker_->PostTask([this] { ApplyProcessing(); });
}

void Engine::SetVadAggressiveness(int mode)
{
    if (mode >= 0) vadMode_ = std::min(mode, 3);
}

json Engine::DeviceList(bool input)
{
    json list = json::array();
    int count = input ? adm_->RecordingDevices() : adm_->PlayoutDevices();
    for (int i = 0; i < count; i++) {
        char name[webrtc::kAdmMaxDeviceNameSize] = {}, guid[webrtc::kAdmMaxGuidSize] = {};
        int rc = input ? adm_->RecordingDeviceName(i, name, guid) : adm_->PlayoutDeviceName(i, name, guid);
        if (rc != 0) continue;
        list.push_back({{"name", name}, {"guid", guid}, {"index", i}});
    }
    return list;
}

json Engine::InputDevices()
{
    return worker_->BlockingCall([&] { return DeviceList(true); });
}

json Engine::OutputDevices()
{
    return worker_->BlockingCall([&] { return DeviceList(false); });
}

// PulseAudio's device 0 is its default device; Windows' devices are only the endpoints, the default one is selected
// by role (and follows the system setting when that changes)
void Engine::SelectDevice(bool input, int index)
{
#ifdef _WIN32
    if (index == kDefaultDevice) {
        if (input) adm_->SetRecordingDevice(webrtc::AudioDeviceModule::kDefaultDevice);
        else adm_->SetPlayoutDevice(webrtc::AudioDeviceModule::kDefaultDevice);
        return;
    }
#endif
    if (input) adm_->SetRecordingDevice(static_cast<uint16_t>(index));
    else adm_->SetPlayoutDevice(static_cast<uint16_t>(index));
}

int Engine::FindDevice(bool input, const std::string& id)
{
    if (id.empty() || id == "default") return kDefaultDevice;
    auto list = DeviceList(input);
    for (auto& d : list)
        if (d["guid"] == id || d["name"] == id) return d["index"].get<int>();
    char* end = nullptr;
    long index = strtol(id.c_str(), &end, 10);
    if (end && *end == 0 && index >= 0 && index < static_cast<long>(list.size())) return static_cast<int>(index);
    VV_LOGW("engine", "device %s not found, using the default", id.c_str());
    return kDefaultDevice;
}

void Engine::SetInputDevice(const std::string& id)
{
    worker_->PostTask([this, id] {
        int index = FindDevice(true, id);
        // Discord sets the device again on every device list change (e.g. a bluetooth headset appearing):
        // restarting the same device would only drop audio
        if (recordingDeviceSet_ && index == recordingDevice_ && id == recordingDeviceId_) return;
        recordingDeviceSet_ = true;
        recordingDeviceId_ = id;
        recordingDevice_ = index;
        bool wasRecording = adm_->Recording();
        if (wasRecording) adm_->StopRecording();
        SelectDevice(true, index);
        adm_->InitMicrophone();
        analyzer_.Reset();
        VV_LOGI("engine", "input device -> %d (%s)", index, id.c_str());
        // restarting here also keeps the mic test running on the new device
        if (wasRecording) UpdateAudioRunning();
    });
}

void Engine::SetOutputDevice(const std::string& id)
{
    worker_->PostTask([this, id] {
        int index = FindDevice(false, id);
        if (playoutDeviceSet_ && index == playoutDevice_ && id == playoutDeviceId_) return;
        playoutDeviceSet_ = true;
        playoutDeviceId_ = id;
        playoutDevice_ = index;
        bool wasPlaying = adm_->Playing();
        if (wasPlaying) adm_->StopPlayout();
        SelectDevice(false, index);
        adm_->InitSpeaker();
        VV_LOGI("engine", "output device -> %d (%s)", index, id.c_str());
        if (wasPlaying) UpdateAudioRunning();
    });
}

void Engine::SetInputVolume(float linear)
{
    adm_->SetInputGain(std::clamp(linear, 0.0f, 4.0f));
}

void Engine::SetOutputVolume(float linear)
{
    mixer_->SetOutputGain(std::clamp(linear, 0.0f, 4.0f));
}

void Engine::PollDevices()
{
    // the noise canceller's cost on the capture thread, every 10 s while one runs
    if (++pollCount_ % 5 == 0) {
        uint64_t frames = adm_->NoiseCancellerFrames(), us = adm_->NoiseCancellerTimeUs();
        if (frames > lastNcFrames_)
            VV_LOGI("engine", "noise canceller: %llu us per 10 ms frame on average", static_cast<unsigned long long>((us - lastNcTimeUs_) / (frames - lastNcFrames_)));
        lastNcFrames_ = frames;
        lastNcTimeUs_ = us;
    }
    if (mixer_->LoopbackEnabled()) {
        auto s = mixer_->TakeLoopbackStats();
        VV_LOGI("engine", "mic test: queue %zu to %zu samples, %llu underruns, %llu dropped in the last 2 s", s.minDepth, s.maxDepth,
                static_cast<unsigned long long>(s.underruns), static_cast<unsigned long long>(s.dropped));
    }
    auto inputs = DeviceList(true);
    auto outputs = DeviceList(false);
    // cameras: the cached list (never touches a device), refreshed in the background only when idle and
    // something was plugged in or out; nothing is reported before the first enumeration is done
    auto videos = Camera::Get().Ready() ? Camera::Get().Devices() : lastVideoInputs_;
    Camera::Get().Refresh();
    if (inputs != lastInputs_ || outputs != lastOutputs_ || videos != lastVideoInputs_) {
        lastInputs_ = inputs;
        lastOutputs_ = outputs;
        lastVideoInputs_ = videos;
        VV_LOGI("engine", "devices changed: %zu inputs, %zu outputs, %zu cameras", inputs.size(), outputs.size(), videos.size());
        callbacks.deviceChange.Call([inputs, outputs, videos](Napi::Env env) {
            return JsCallback::Args{FromJson(env, inputs), FromJson(env, outputs), FromJson(env, videos)};
        });
    }
    worker_->PostDelayedTask([this] { PollDevices(); }, webrtc::TimeDelta::Seconds(2));
}

void Engine::SetEmitVadLevel(bool enabled)
{
    emitVadLevel_ = enabled;
    worker_->PostTask([this] { UpdateAudioRunning(); });
}

void Engine::SetLoopback(bool enabled, const json& options)
{
    {
        std::lock_guard lock(mutex_);
        if (enabled) {
            ProcessingOptions o = options_;
            o.Update(options);
            loopbackOptions_ = o;
        } else {
            loopbackOptions_.reset();
        }
    }
    mixer_->SetLoopback(enabled);
    VV_LOGI("engine", "loopback %s", enabled ? "on" : "off");
    worker_->PostTask([this] { UpdateAudioRunning(); });
}

void Engine::OnProcessedCapture(const float* samples, size_t frames)
{
    int64_t now = NowMs();
    if (!firstCaptureMs_) firstCaptureMs_ = now;
    framesCaptured_++;

    // Without a sending connection (the mic test, a voice message) libwebrtc captures at a lower rate than a call's
    // 48 kHz (16 kHz). Everything below is 48 kHz: the mixer plays the loopback at that rate and the WAV says so
    float resampled[480];
    int rate = captureRateHz_;
    if (rate != 48000 && rate > 0 && frames * 100 == static_cast<size_t>(rate)) {
        if (!captureResampler_ || captureResamplerFrames_ != frames) {
            captureResampler_ = std::make_unique<webrtc::PushResampler<float>>(frames, 480, 1);
            captureResamplerFrames_ = frames;
        }
        captureResampler_->Resample(webrtc::MonoView<const float>(samples, frames), webrtc::MonoView<float>(resampled, 480));
        samples = resampled;
        frames = 480;
    }

    float norm[960];
    int16_t pcm[960];
    frames = std::min<size_t>(frames, 960);
    for (size_t i = 0; i < frames; i++) {
        norm[i] = samples[i] / 32768.0f;
        pcm[i] = static_cast<int16_t>(std::clamp(samples[i], -32768.0f, 32767.0f));
    }

    CaptureFrameInfo info;
    info.timeMs = now;
    info.levelDb = analyzer_.Process(norm, frames);
    int mode = vadMode_;
    if (mode != appliedVadMode_) {
        WebRtcVad_set_mode(static_cast<WebRtcVadInst*>(vad_), mode);
        appliedVadMode_ = mode;
    }
    if (frames == 480 || frames == 960)
        info.webrtcVad = WebRtcVad_Process(static_cast<WebRtcVadInst*>(vad_), 48000, pcm, frames) == 1;
    info.voiceProbability = adm_->VoiceProbability();

    mixer_->PushLoopback(samples, frames);
    WriteRecording(norm, frames);

    int flags = 0;
    {
        std::lock_guard lock(connectionsMutex_);
        for (auto* c : connections_) {
            c->OnCaptureFrame(info);
            if (c->Speaking()) flags = 1;
        }
    }

    if (emitVadLevel_ && now - lastVoiceEmitMs_ >= 100) {
        lastVoiceEmitMs_ = now;
        float level = info.levelDb;
        callbacks.onVoice.Call([level, flags](Napi::Env env) {
            return JsCallback::Args{Napi::Number::New(env, level), Napi::Number::New(env, flags)};
        });
    }

    // "no input detected": nothing above the threshold for 5 s
    if (info.levelDb > noInputThresholdDb_) lastInputMs_ = now;
    int state = now - std::max(lastInputMs_, static_cast<int64_t>(firstCaptureMs_)) < 5000 ? 1 : 0;
    if (lastInputMs_ == 0 && now - firstCaptureMs_ < 5000) state = -1;
    if (state >= 0 && state != noInputState_) {
        noInputState_ = state;
        callbacks.noInput.Call([state](Napi::Env env) { return JsCallback::Args{Napi::Boolean::New(env, state == 1)}; });
    }
}

bool Engine::StartLocalRecording(const json& options)
{
    {
        std::lock_guard lock(mutex_);
        ProcessingOptions o = options_;
        o.Update(options);
        recordingOptions_ = o;
    }
    {
        std::lock_guard lock(recordingMutex_);
        if (recordingFile_) fclose(recordingFile_);
        recordingPath_ = PathToUtf8(PathFromUtf8(dataDirectory_) / ("voice-message-" + std::to_string(NowMs()) + ".wav"));
        recordingFile_ = OpenFile(recordingPath_, "wb");
        recordingFrames_ = 0;
        if (!recordingFile_) return false;
        uint8_t header[44] = {};
        fwrite(header, 1, sizeof(header), recordingFile_); // patched in StopLocalRecording
    }
    worker_->PostTask([this] { UpdateAudioRunning(); });
    return true;
}

void Engine::WriteRecording(const float* samples, size_t frames)
{
    std::lock_guard lock(recordingMutex_);
    if (!recordingFile_) return;
    int16_t pcm[960];
    for (size_t i = 0; i < frames; i++) pcm[i] = static_cast<int16_t>(std::clamp(samples[i], -1.0f, 1.0f) * 32767);
    fwrite(pcm, 2, frames, recordingFile_);
    recordingFrames_ += frames;
}

std::pair<std::string, int64_t> Engine::StopLocalRecording()
{
    std::string path;
    int64_t durationMs = 0;
    {
        std::lock_guard lock(recordingMutex_);
        if (!recordingFile_) return {"", 0};
        uint32_t dataBytes = static_cast<uint32_t>(recordingFrames_ * 2);
        auto put32 = [](uint8_t* p, uint32_t v) { memcpy(p, &v, 4); };
        auto put16 = [](uint8_t* p, uint16_t v) { memcpy(p, &v, 2); };
        uint8_t h[44];
        memcpy(h, "RIFF", 4), put32(h + 4, 36 + dataBytes), memcpy(h + 8, "WAVEfmt ", 8);
        put32(h + 16, 16), put16(h + 20, 1), put16(h + 22, 1), put32(h + 24, 48000), put32(h + 28, 96000);
        put16(h + 32, 2), put16(h + 34, 16), memcpy(h + 36, "data", 4), put32(h + 40, dataBytes);
        fseek(recordingFile_, 0, SEEK_SET);
        fwrite(h, 1, sizeof(h), recordingFile_);
        fclose(recordingFile_);
        recordingFile_ = nullptr;
        path = recordingPath_;
        durationMs = static_cast<int64_t>(recordingFrames_ / 48);
    }
    {
        std::lock_guard lock(mutex_);
        recordingOptions_.reset();
    }
    worker_->PostTask([this] { UpdateAudioRunning(); });
    return {path, durationMs};
}

Engine::Stats Engine::GetStats() const
{
    Stats s;
    s.framesCaptured = framesCaptured_;
    s.firstCaptureMs = firstCaptureMs_;
    if (adm_) {
        s.noiseCancellerFrames = adm_->NoiseCancellerFrames();
        s.noiseCancellerTimeUs = adm_->NoiseCancellerTimeUs();
    }
    return s;
}

} // namespace vv
