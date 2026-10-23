// Playout mixer (WebRTC AudioMixer). Mixes every remote audio receive stream of every connection with
// per-SSRC pan / mute (setLocalPan, deafen), plus the mic test loopback and local sample playback
// (soundboard), and applies the output volume.
#pragma once

#include <api/audio/audio_mixer.h>
#include <common_audio/resampler/include/push_resampler.h>

#include <deque>
#include <memory>
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
    // Adds the next 10 ms of the loopback to `data` (interleaved int16, 48 kHz), after the echo reference was taken
    void MixLoopbackInto(int16_t* data, size_t frames, size_t channels, uint32_t rate);
    // Silences the loopback for `frames` x 10 ms (with short ramps) while freshly reconfigured processing settles
    void DuckLoopback(int frames);
    struct LoopbackStats {
        uint64_t underruns = 0, dropped = 0;
        size_t minDepth = 0, maxDepth = 0;
    };
    // Since the last call
    LoopbackStats TakeLoopbackStats();

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
    bool loopbackPrimed_ = false;   // buffered enough to start playing; false again after an underrun
    int duckFrames_ = 0;            // frames of silence left
    float duckGain_ = 1;            // the gain at the end of the last frame
    std::unique_ptr<webrtc::PushResampler<float>> loopbackResampler_;
    size_t loopbackResamplerFrames_ = 0;
    LoopbackStats stats_;
    bool statsFresh_ = true;
    std::map<std::string, Samples> samples_;
};

} // namespace vv
