#include "audio/adm.h"

#include <api/make_ref_counted.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "audio/mixer.h"
#include "common/log.h"
#include "engine/engine.h"
#include "common/util.h"

namespace vv {

using webrtc::AudioDeviceModule;

// ---------------------------------------------------------------------------------------------- CaptureAdm

webrtc::scoped_refptr<CaptureAdm> CaptureAdm::Create(webrtc::scoped_refptr<AudioDeviceModule> inner)
{
    return webrtc::make_ref_counted<CaptureAdm>(std::move(inner));
}

CaptureAdm::CaptureAdm(webrtc::scoped_refptr<AudioDeviceModule> inner) : inner_(std::move(inner))
{
    inner_->RegisterAudioCallback(&tap_);
}

CaptureAdm::~CaptureAdm()
{
    inner_->RegisterAudioCallback(nullptr);
}

int32_t CaptureAdm::RegisterAudioCallback(webrtc::AudioTransport* callback)
{
    std::lock_guard lock(mutex_);
    downstream_ = callback;
    return 0;
}

void CaptureAdm::Process(int16_t* samples, size_t frames, size_t channels, uint32_t rate)
{
    float gain = gain_;
    if (gain != 1.0f)
        for (size_t i = 0; i < frames * channels; i++)
            samples[i] = static_cast<int16_t>(std::clamp(samples[i] * gain, -32768.0f, 32767.0f));

    Denoiser denoiser = denoiser_;
    if (resetDenoiser_.exchange(false)) {
        for (auto& r : rnnoise_) r.reset();
        // a DeepFilterNet state is freed (and its creation waited for) away from the audio thread
        for (auto& d : deepFilter_)
            if (d) std::thread([old = std::move(d)] {}).detach();
    }
    if (denoiser == Denoiser::None || rate != 48000 || frames != 480 || channels > 2) {
        voiceProbability_ = -1;
        return;
    }
    int64_t t0 = NowUs();
    float prob = 0;
    bool processed = false;
    for (size_t c = 0; c < channels; c++) {
        float buf[480];
        for (size_t i = 0; i < frames; i++) buf[i] = samples[i * channels + c];
        if (denoiser == Denoiser::RnNoise) {
            if (!rnnoise_[c]) rnnoise_[c] = std::make_unique<RnNoise>();
            if (!*rnnoise_[c]) {
                voiceProbability_ = -1;
                return;
            }
            prob = std::max(prob, rnnoise_[c]->Process(buf));
        } else {
            // when it cannot be created the engine falls back to the standard suppression
            if (!deepFilter_[c]) deepFilter_[c] = std::make_unique<DeepFilter>([] { Engine::Get().OnDenoiserFailed(); });
            if (!deepFilter_[c]->Process(buf)) continue; // not ready yet: unchanged
            prob = -1; // no speech probability: voice activity falls back to the WebRTC VAD
        }
        processed = true;
        for (size_t i = 0; i < frames; i++) samples[i * channels + c] = static_cast<int16_t>(std::clamp(buf[i], -32768.0f, 32767.0f));
    }
    voiceProbability_ = denoiser == Denoiser::RnNoise ? prob : -1;
    if (!processed) return;
    ncFrames_++;
    ncTimeUs_ += NowUs() - t0;
}

int32_t CaptureAdm::RecordedDataIsAvailable(const void* samples, size_t nSamples, size_t bytesPerSample, size_t channels,
                                            uint32_t rate, uint32_t delayMs, int32_t drift, uint32_t micLevel,
                                            bool keyPressed, uint32_t& newMicLevel)
{
    return RecordedDataIsAvailable(samples, nSamples, bytesPerSample, channels, rate, delayMs, drift, micLevel, keyPressed,
                                   newMicLevel, std::nullopt);
}

int32_t CaptureAdm::RecordedDataIsAvailable(const void* samples, size_t nSamples, size_t bytesPerSample, size_t channels,
                                            uint32_t rate, uint32_t delayMs, int32_t drift, uint32_t micLevel,
                                            bool keyPressed, uint32_t& newMicLevel, std::optional<int64_t> captureTimeNs)
{
    std::lock_guard lock(mutex_);
    if (!downstream_) return 0;
    const void* data = samples;
    if (bytesPerSample == 2 * channels) {
        buffer_.assign(static_cast<const int16_t*>(samples), static_cast<const int16_t*>(samples) + nSamples * channels);
        Process(buffer_.data(), nSamples, channels, rate);
        data = buffer_.data();
    }
    return downstream_->RecordedDataIsAvailable(data, nSamples, bytesPerSample, channels, rate, delayMs, drift, micLevel,
                                                keyPressed, newMicLevel, captureTimeNs);
}

int32_t CaptureAdm::NeedMorePlayData(size_t nSamples, size_t bytesPerSample, size_t channels, uint32_t rate, void* samples,
                                     size_t& samplesOut, int64_t* elapsedMs, int64_t* ntpMs)
{
    std::lock_guard lock(mutex_);
    if (!downstream_) {
        memset(samples, 0, nSamples * bytesPerSample);
        samplesOut = nSamples;
        return 0;
    }
    int32_t rc = downstream_->NeedMorePlayData(nSamples, bytesPerSample, channels, rate, samples, samplesOut, elapsedMs, ntpMs);
    // the mic test's own voice goes to the speakers but not to the echo canceller's reference
    if (rc == 0 && bytesPerSample == 2 * channels && samplesOut >= nSamples)
        Engine::Get().PlayoutMixer().MixLoopbackInto(static_cast<int16_t*>(samples), nSamples, channels, rate);
    return rc;
}

void CaptureAdm::PullRenderData(int bits, int rate, size_t channels, size_t frames, void* data, int64_t* elapsedMs,
                                int64_t* ntpMs)
{
    std::lock_guard lock(mutex_);
    if (downstream_) downstream_->PullRenderData(bits, rate, channels, frames, data, elapsedMs, ntpMs);
}

// ---------------------------------------------------------------------------------------------- FakeAdm

namespace {

class FakeAdm : public AudioDeviceModule {
public:
    ~FakeAdm() override { Terminate(); }

    int32_t ActiveAudioLayer(AudioLayer* layer) const override
    {
        *layer = kDummyAudio;
        return 0;
    }
    int32_t RegisterAudioCallback(webrtc::AudioTransport* cb) override
    {
        std::lock_guard lock(mutex_);
        transport_ = cb;
        return 0;
    }
    int32_t Init() override
    {
        if (running_) return 0;
        running_ = true;
        thread_ = std::thread([this] { Run(); });
        return 0;
    }
    int32_t Terminate() override
    {
        running_ = false;
        if (thread_.joinable()) thread_.join();
        return 0;
    }
    bool Initialized() const override { return running_; }
    int16_t PlayoutDevices() override { return 2; }
    int16_t RecordingDevices() override { return 2; }
    int32_t PlayoutDeviceName(uint16_t i, char name[webrtc::kAdmMaxDeviceNameSize], char guid[webrtc::kAdmMaxGuidSize]) override
    {
        return Name(i, "Fake Audio Output", name, guid);
    }
    int32_t RecordingDeviceName(uint16_t i, char name[webrtc::kAdmMaxDeviceNameSize], char guid[webrtc::kAdmMaxGuidSize]) override
    {
        return Name(i, "Fake Audio Input", name, guid);
    }
    int32_t SetPlayoutDevice(uint16_t) override { return 0; }
    int32_t SetPlayoutDevice(WindowsDeviceType) override { return 0; }
    int32_t SetRecordingDevice(uint16_t) override { return 0; }
    int32_t SetRecordingDevice(WindowsDeviceType) override { return 0; }
    int32_t PlayoutIsAvailable(bool* a) override { return *a = true, 0; }
    int32_t InitPlayout() override { return playoutInit_ = true, 0; }
    bool PlayoutIsInitialized() const override { return playoutInit_; }
    int32_t RecordingIsAvailable(bool* a) override { return *a = true, 0; }
    int32_t InitRecording() override { return recordingInit_ = true, 0; }
    bool RecordingIsInitialized() const override { return recordingInit_; }
    int32_t StartPlayout() override { return playing_ = true, 0; }
    int32_t StopPlayout() override { return playing_ = false, playoutInit_ = false, 0; }
    bool Playing() const override { return playing_; }
    int32_t StartRecording() override { return recording_ = true, 0; }
    int32_t StopRecording() override { return recording_ = false, recordingInit_ = false, 0; }
    bool Recording() const override { return recording_; }
    int32_t InitSpeaker() override { return 0; }
    bool SpeakerIsInitialized() const override { return true; }
    int32_t InitMicrophone() override { return 0; }
    bool MicrophoneIsInitialized() const override { return true; }
    int32_t SpeakerVolumeIsAvailable(bool* a) override { return *a = false, 0; }
    int32_t SetSpeakerVolume(uint32_t) override { return -1; }
    int32_t SpeakerVolume(uint32_t*) const override { return -1; }
    int32_t MaxSpeakerVolume(uint32_t*) const override { return -1; }
    int32_t MinSpeakerVolume(uint32_t*) const override { return -1; }
    int32_t MicrophoneVolumeIsAvailable(bool* a) override { return *a = false, 0; }
    int32_t SetMicrophoneVolume(uint32_t) override { return -1; }
    int32_t MicrophoneVolume(uint32_t*) const override { return -1; }
    int32_t MaxMicrophoneVolume(uint32_t*) const override { return -1; }
    int32_t MinMicrophoneVolume(uint32_t*) const override { return -1; }
    int32_t SpeakerMuteIsAvailable(bool* a) override { return *a = false, 0; }
    int32_t SetSpeakerMute(bool) override { return -1; }
    int32_t SpeakerMute(bool*) const override { return -1; }
    int32_t MicrophoneMuteIsAvailable(bool* a) override { return *a = false, 0; }
    int32_t SetMicrophoneMute(bool) override { return -1; }
    int32_t MicrophoneMute(bool*) const override { return -1; }
    int32_t StereoPlayoutIsAvailable(bool* a) const override { return *a = true, 0; }
    int32_t SetStereoPlayout(bool) override { return 0; }
    int32_t StereoPlayout(bool* e) const override { return *e = true, 0; }
    int32_t StereoRecordingIsAvailable(bool* a) const override { return *a = false, 0; }
    int32_t SetStereoRecording(bool) override { return 0; }
    int32_t StereoRecording(bool* e) const override { return *e = false, 0; }
    int32_t PlayoutDelay(uint16_t* d) const override { return *d = 10, 0; }
    bool BuiltInAECIsAvailable() const override { return false; }
    bool BuiltInAGCIsAvailable() const override { return false; }
    bool BuiltInNSIsAvailable() const override { return false; }
    int32_t EnableBuiltInAEC(bool) override { return -1; }
    int32_t EnableBuiltInAGC(bool) override { return -1; }
    int32_t EnableBuiltInNS(bool) override { return -1; }

private:
    static int32_t Name(uint16_t i, const char* base, char* name, char* guid)
    {
        if (i > 1) return -1;
        snprintf(name, webrtc::kAdmMaxDeviceNameSize, i == 0 ? "default: %s" : "%s", base);
        snprintf(guid, webrtc::kAdmMaxGuidSize, "%s", i == 0 ? "" : base);
        return 0;
    }

    void Run()
    {
        using namespace std::chrono;
        auto next = steady_clock::now();
        int16_t mic[480];
        int16_t speaker[480 * 2];
        uint64_t n = 0;
        while (running_) {
            next += milliseconds(10);
            std::this_thread::sleep_until(next);
            std::lock_guard lock(mutex_);
            if (!transport_) continue;
            if (recording_) {
                // A 440 Hz tone with a 2 Hz tremolo, about -20 dBFS: always "speech" for level based VAD
                if (!fileSamples_.empty()) {
                    // VESKTOP_MEDIA_FAKE_MIC=file:<path>: a 48 kHz mono 16-bit WAV, looped
                    for (int i = 0; i < 480; i++, n++) mic[i] = fileSamples_[n % fileSamples_.size()];
                } else if (speechLike_) {
                    // VESKTOP_MEDIA_FAKE_MIC=speech: a noise floor (about -55 dBFS) with voiced bursts (150 Hz and its
                    // harmonics, 300 ms on, 200 ms off, about -20 dBFS): something the noise suppressors treat as a voice
                    for (int i = 0; i < 480; i++, n++) {
                        double t = static_cast<double>(n) / 48000.0;
                        double phase = std::fmod(t, 0.5);
                        double env = phase < 0.3 ? std::sin(M_PI * phase / 0.3) : 0.0;
                        double voice = 0;
                        for (int k = 1; k <= 20; k++) voice += std::sin(2 * M_PI * 150 * k * t) / k;
                        noise_ = 0.97 * noise_ + 0.03 * ((rand() / static_cast<double>(RAND_MAX)) - 0.5);
                        mic[i] = static_cast<int16_t>(std::clamp(2500 * env * voice + 9000 * noise_, -32768.0, 32767.0));
                    }
                } else {
                    for (int i = 0; i < 480; i++, n++) {
                        double t = static_cast<double>(n) / 48000.0;
                        double env = 0.6 + 0.4 * std::sin(2 * M_PI * 2 * t);
                        mic[i] = static_cast<int16_t>(3200 * env * std::sin(2 * M_PI * 440 * t));
                    }
                }
                uint32_t newLevel = 0;
                transport_->RecordedDataIsAvailable(mic, 480, 2, 1, 48000, 20, 0, 0, false, newLevel);
            }
            if (playing_) {
                size_t out = 0;
                int64_t elapsed = 0, ntp = 0;
                transport_->NeedMorePlayData(480, 4, 2, 48000, speaker, out, &elapsed, &ntp);
            }
        }
    }

    const bool speechLike_ = getenv("VESKTOP_MEDIA_FAKE_MIC") && std::string(getenv("VESKTOP_MEDIA_FAKE_MIC")) == "speech";
    const std::vector<int16_t> fileSamples_ = [] {
        std::vector<int16_t> samples;
        const char* env = getenv("VESKTOP_MEDIA_FAKE_MIC");
        if (!env || strncmp(env, "file:", 5) != 0) return samples;
        if (FILE* f = fopen(env + 5, "rb")) {
            fseek(f, 44, SEEK_SET); // a plain WAV header
            int16_t buf[4096];
            size_t n;
            while ((n = fread(buf, 2, 4096, f)) > 0) samples.insert(samples.end(), buf, buf + n);
            fclose(f);
        }
        return samples;
    }();
    double noise_ = 0;
    std::mutex mutex_;
    webrtc::AudioTransport* transport_ = nullptr;
    std::atomic<bool> running_{false}, playing_{false}, recording_{false}, playoutInit_{false}, recordingInit_{false};
    std::thread thread_;
};

} // namespace

webrtc::scoped_refptr<AudioDeviceModule> CreateFakeAdm()
{
    return webrtc::make_ref_counted<FakeAdm>();
}

} // namespace vv
