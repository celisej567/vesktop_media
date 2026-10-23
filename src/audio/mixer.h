// Playout mixer (WebRTC AudioMixer). Mixes every remote audio receive stream of every connection with
// per-SSRC pan / mute (setLocalPan, deafen), plus the mic test loopback and local sample playback
// (soundboard), and applies the output volume.
#pragma once

#include <api/audio/audio_mixer.h>

#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace vv {

class Mixer : public webrtc::AudioMixer {
public:
    struct SourceGain {
        float left = 1, right = 1;
        bool muted = false;
    };

    void SetSourceGain(uint32_t ssrc, SourceGain gain);
    void RemoveSourceGain(uint32_t ssrc);
    void SetOutputGain(float gain);

    // Mic test: processed capture (int16 range floats, mono) played back
    void SetLoopback(bool enabled);
    bool LoopbackEnabled() const;
    void PushLoopback(const float* samples, size_t count);

    // channels: 1 or 2 planes of float samples at `sampleRate`. `done` is called when it finished or was stopped
    void StartSamples(const std::string& key, int sampleRate, float volume, std::vector<std::vector<float>> channels,
                      std::function<void()> done);
    void StopSamples(const std::string& key);
    void StopSamplesWithPrefix(const std::string& prefix);
    bool HasSamples() const;

    // AudioMixer
    bool AddSource(Source* source) override;
    void RemoveSource(Source* source) override;
    void Mix(size_t channels, webrtc::AudioFrame* frame) override;

private:
    struct Samples {
        std::vector<float> stereo; // interleaved 48 kHz
        size_t pos = 0;
        float volume = 1;
        std::function<void()> done;
    };

    mutable std::mutex mutex_;
    std::vector<Source*> sources_;
    std::map<uint32_t, SourceGain> gains_;
    float outputGain_ = 1;
    bool loopback_ = false;
    std::deque<float> loopbackQueue_;
    std::map<std::string, Samples> samples_;
};

} // namespace vv
