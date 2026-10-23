#include "audio/adm.h"

#include <api/make_ref_counted.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "common/log.h"
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

    if (!noiseCancellation_ || rate != 48000 || frames != 480 || channels > 2) {
        voiceProbability_ = -1;
        return;
    }
    int64_t t0 = NowUs();
    float prob = 0;
    for (size_t c = 0; c < channels; c++) {
        if (!rnnoise_[c]) rnnoise_[c] = std::make_unique<RnNoise>();
        if (!*rnnoise_[c]) {
            voiceProbability_ = -1;
            return;
        }
        float buf[480];
        for (size_t i = 0; i < frames; i++) buf[i] = samples[i * channels + c];
        prob = std::max(prob, rnnoise_[c]->Process(buf));
        for (size_t i = 0; i < frames; i++) samples[i * channels + c] = static_cast<int16_t>(std::clamp(buf[i], -32768.0f, 32767.0f));
    }
    voiceProbability_ = prob;
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
    return downstream_->NeedMorePlayData(nSamples, bytesPerSample, channels, rate, samples, samplesOut, elapsedMs, ntpMs);
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
                for (int i = 0; i < 480; i++, n++) {
                    double t = static_cast<double>(n) / 48000.0;
                    double env = 0.6 + 0.4 * std::sin(2 * M_PI * 2 * t);
                    mic[i] = static_cast<int16_t>(3200 * env * std::sin(2 * M_PI * 440 * t));
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
