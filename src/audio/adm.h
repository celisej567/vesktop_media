// Audio device modules.
//
//  * CaptureAdm wraps the platform ADM (WebRTC's PulseAudio / ALSA module on Linux) and processes the
//    recorded audio before it reaches WebRTC: input volume and the noise canceller (RNNoise).
//  * FakeAdm is the "useFakeAudioCapture" device: a synthetic mic and a discarding speaker on their own
//    10 ms clock, so tests need no sound hardware.
#pragma once

#include <api/audio/audio_device.h>
#include <api/scoped_refptr.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "audio/deepfilter.h"
#include "audio/rnnoise.h"

namespace vv {

webrtc::scoped_refptr<webrtc::AudioDeviceModule> CreateFakeAdm();

class CaptureAdm : public webrtc::AudioDeviceModule {
public:
    static webrtc::scoped_refptr<CaptureAdm> Create(webrtc::scoped_refptr<webrtc::AudioDeviceModule> inner);

    void SetInputGain(float gain) { gain_ = gain; }
    // The noise canceller that runs on the capture before libwebrtc's processing. Switching one on starts from a
    // fresh state: an old one has the last speech of whenever it was running
    enum class Denoiser { None, RnNoise, DeepFilter };
    void SetDenoiser(Denoiser denoiser)
    {
        if (denoiser_.exchange(denoiser) != denoiser && denoiser != Denoiser::None) resetDenoiser_ = true;
    }
    bool NoiseCancellationAvailable() const { return RnNoise::Available(); }
    // speech probability of the last frame, -1 when the noise canceller is off
    float VoiceProbability() const { return voiceProbability_; }
    uint64_t NoiseCancellerFrames() const { return ncFrames_; }
    uint64_t NoiseCancellerTimeUs() const { return ncTimeUs_; }
    webrtc::AudioDeviceModule* Inner() const { return inner_.get(); }

    // AudioDeviceModule
    int32_t ActiveAudioLayer(AudioLayer* layer) const override { return inner_->ActiveAudioLayer(layer); }
    int32_t RegisterAudioCallback(webrtc::AudioTransport* callback) override;
    int32_t Init() override { return inner_->Init(); }
    int32_t Terminate() override { return inner_->Terminate(); }
    bool Initialized() const override { return inner_->Initialized(); }
    int16_t PlayoutDevices() override { return inner_->PlayoutDevices(); }
    int16_t RecordingDevices() override { return inner_->RecordingDevices(); }
    int32_t PlayoutDeviceName(uint16_t i, char name[webrtc::kAdmMaxDeviceNameSize], char guid[webrtc::kAdmMaxGuidSize]) override
    {
        return inner_->PlayoutDeviceName(i, name, guid);
    }
    int32_t RecordingDeviceName(uint16_t i, char name[webrtc::kAdmMaxDeviceNameSize], char guid[webrtc::kAdmMaxGuidSize]) override
    {
        return inner_->RecordingDeviceName(i, name, guid);
    }
    int32_t SetPlayoutDevice(uint16_t i) override { return inner_->SetPlayoutDevice(i); }
    int32_t SetPlayoutDevice(WindowsDeviceType d) override { return inner_->SetPlayoutDevice(d); }
    int32_t SetRecordingDevice(uint16_t i) override { return inner_->SetRecordingDevice(i); }
    int32_t SetRecordingDevice(WindowsDeviceType d) override { return inner_->SetRecordingDevice(d); }
    int32_t PlayoutIsAvailable(bool* a) override { return inner_->PlayoutIsAvailable(a); }
    int32_t InitPlayout() override { return inner_->InitPlayout(); }
    bool PlayoutIsInitialized() const override { return inner_->PlayoutIsInitialized(); }
    int32_t RecordingIsAvailable(bool* a) override { return inner_->RecordingIsAvailable(a); }
    int32_t InitRecording() override { return inner_->InitRecording(); }
    bool RecordingIsInitialized() const override { return inner_->RecordingIsInitialized(); }
    int32_t StartPlayout() override { return inner_->StartPlayout(); }
    int32_t StopPlayout() override { return inner_->StopPlayout(); }
    bool Playing() const override { return inner_->Playing(); }
    int32_t StartRecording() override { return inner_->StartRecording(); }
    int32_t StopRecording() override { return inner_->StopRecording(); }
    bool Recording() const override { return inner_->Recording(); }
    int32_t InitSpeaker() override { return inner_->InitSpeaker(); }
    bool SpeakerIsInitialized() const override { return inner_->SpeakerIsInitialized(); }
    int32_t InitMicrophone() override { return inner_->InitMicrophone(); }
    bool MicrophoneIsInitialized() const override { return inner_->MicrophoneIsInitialized(); }
    int32_t SpeakerVolumeIsAvailable(bool* a) override { return inner_->SpeakerVolumeIsAvailable(a); }
    int32_t SetSpeakerVolume(uint32_t v) override { return inner_->SetSpeakerVolume(v); }
    int32_t SpeakerVolume(uint32_t* v) const override { return inner_->SpeakerVolume(v); }
    int32_t MaxSpeakerVolume(uint32_t* v) const override { return inner_->MaxSpeakerVolume(v); }
    int32_t MinSpeakerVolume(uint32_t* v) const override { return inner_->MinSpeakerVolume(v); }
    int32_t MicrophoneVolumeIsAvailable(bool* a) override { return inner_->MicrophoneVolumeIsAvailable(a); }
    int32_t SetMicrophoneVolume(uint32_t v) override { return inner_->SetMicrophoneVolume(v); }
    int32_t MicrophoneVolume(uint32_t* v) const override { return inner_->MicrophoneVolume(v); }
    int32_t MaxMicrophoneVolume(uint32_t* v) const override { return inner_->MaxMicrophoneVolume(v); }
    int32_t MinMicrophoneVolume(uint32_t* v) const override { return inner_->MinMicrophoneVolume(v); }
    int32_t SpeakerMuteIsAvailable(bool* a) override { return inner_->SpeakerMuteIsAvailable(a); }
    int32_t SetSpeakerMute(bool e) override { return inner_->SetSpeakerMute(e); }
    int32_t SpeakerMute(bool* e) const override { return inner_->SpeakerMute(e); }
    int32_t MicrophoneMuteIsAvailable(bool* a) override { return inner_->MicrophoneMuteIsAvailable(a); }
    int32_t SetMicrophoneMute(bool e) override { return inner_->SetMicrophoneMute(e); }
    int32_t MicrophoneMute(bool* e) const override { return inner_->MicrophoneMute(e); }
    int32_t StereoPlayoutIsAvailable(bool* a) const override { return inner_->StereoPlayoutIsAvailable(a); }
    int32_t SetStereoPlayout(bool e) override { return inner_->SetStereoPlayout(e); }
    int32_t StereoPlayout(bool* e) const override { return inner_->StereoPlayout(e); }
    int32_t StereoRecordingIsAvailable(bool* a) const override { return inner_->StereoRecordingIsAvailable(a); }
    int32_t SetStereoRecording(bool e) override { return inner_->SetStereoRecording(e); }
    int32_t StereoRecording(bool* e) const override { return inner_->StereoRecording(e); }
    int32_t PlayoutDelay(uint16_t* d) const override { return inner_->PlayoutDelay(d); }
    bool BuiltInAECIsAvailable() const override { return false; }
    bool BuiltInAGCIsAvailable() const override { return false; }
    bool BuiltInNSIsAvailable() const override { return false; }
    int32_t EnableBuiltInAEC(bool) override { return -1; }
    int32_t EnableBuiltInAGC(bool) override { return -1; }
    int32_t EnableBuiltInNS(bool) override { return -1; }
    int32_t GetPlayoutUnderrunCount() const override { return inner_->GetPlayoutUnderrunCount(); }
    std::optional<Stats> GetStats() const override { return inner_->GetStats(); }

    explicit CaptureAdm(webrtc::scoped_refptr<webrtc::AudioDeviceModule> inner);
    ~CaptureAdm() override;

private:
    // AudioTransport of the inner ADM
    class Tap : public webrtc::AudioTransport {
    public:
        explicit Tap(CaptureAdm* owner) : o_(owner) {}
        int32_t RecordedDataIsAvailable(const void* s, size_t n, size_t b, size_t c, uint32_t r, uint32_t d, int32_t dr,
                                        uint32_t m, bool k, uint32_t& nm) override
        {
            return o_->RecordedDataIsAvailable(s, n, b, c, r, d, dr, m, k, nm, std::nullopt);
        }
        int32_t RecordedDataIsAvailable(const void* s, size_t n, size_t b, size_t c, uint32_t r, uint32_t d, int32_t dr,
                                        uint32_t m, bool k, uint32_t& nm, std::optional<int64_t> t) override
        {
            return o_->RecordedDataIsAvailable(s, n, b, c, r, d, dr, m, k, nm, t);
        }
        int32_t NeedMorePlayData(size_t n, size_t b, size_t c, uint32_t r, void* s, size_t& o, int64_t* e, int64_t* t) override
        {
            return o_->NeedMorePlayData(n, b, c, r, s, o, e, t);
        }
        void PullRenderData(int bits, int r, size_t c, size_t f, void* d, int64_t* e, int64_t* t) override
        {
            o_->PullRenderData(bits, r, c, f, d, e, t);
        }

    private:
        CaptureAdm* o_;
    } tap_{this};

    int32_t RecordedDataIsAvailable(const void* samples, size_t nSamples, size_t bytesPerSample, size_t channels,
                                    uint32_t rate, uint32_t delayMs, int32_t drift, uint32_t micLevel, bool keyPressed,
                                    uint32_t& newMicLevel) ;
    int32_t RecordedDataIsAvailable(const void* samples, size_t nSamples, size_t bytesPerSample, size_t channels,
                                    uint32_t rate, uint32_t delayMs, int32_t drift, uint32_t micLevel, bool keyPressed,
                                    uint32_t& newMicLevel, std::optional<int64_t> captureTimeNs);
    int32_t NeedMorePlayData(size_t nSamples, size_t bytesPerSample, size_t channels, uint32_t rate, void* samples,
                             size_t& samplesOut, int64_t* elapsedMs, int64_t* ntpMs);
    void PullRenderData(int bits, int rate, size_t channels, size_t frames, void* data, int64_t* elapsedMs,
                        int64_t* ntpMs);

    void Process(int16_t* samples, size_t frames, size_t channels, uint32_t rate);

    webrtc::scoped_refptr<webrtc::AudioDeviceModule> inner_;
    std::mutex mutex_;
    webrtc::AudioTransport* downstream_ = nullptr;
    std::vector<int16_t> buffer_;
    std::atomic<float> gain_{1.0f};
    std::atomic<Denoiser> denoiser_{Denoiser::None};
    std::atomic<bool> resetDenoiser_{false};
    std::atomic<float> voiceProbability_{-1};
    std::atomic<uint64_t> ncFrames_{0}, ncTimeUs_{0};
    std::unique_ptr<RnNoise> rnnoise_[2];
    std::unique_ptr<DeepFilter> deepFilter_[2];
};

} // namespace vv
