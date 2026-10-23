#include "audio/mixer.h"

#include <api/audio/audio_frame.h>

#include <algorithm>
#include <cmath>

namespace vv {

namespace {
constexpr size_t kFrames = 480; // 10 ms @ 48 kHz
constexpr size_t kMaxLoopbackQueue = 48000 / 5; // 200 ms
constexpr size_t kLoopbackPrime = kFrames * 3;   // 30 ms
} // namespace

void Mixer::SetSourceGain(uint32_t ssrc, SourceGain gain)
{
    std::lock_guard lock(mutex_);
    gains_[ssrc] = gain;
}

void Mixer::RemoveSourceGain(uint32_t ssrc)
{
    std::lock_guard lock(mutex_);
    gains_.erase(ssrc);
}

void Mixer::SetOutputGain(float gain)
{
    std::lock_guard lock(mutex_);
    outputGain_ = gain;
}

void Mixer::SetLoopback(bool enabled)
{
    std::lock_guard lock(mutex_);
    // Discord sends the loopback options again with every audio setting, often several times in a row: only
    // turning it on starts a fresh queue, repeating it must not drop what is queued (an audible glitch)
    if (enabled == loopback_) return;
    loopback_ = enabled;
    loopbackQueue_.clear();
    loopbackPrimed_ = false;
    duckFrames_ = 0;
    duckGain_ = 1;
    stats_ = {};
    statsFresh_ = true;
}

void Mixer::DuckLoopback(int frames)
{
    std::lock_guard lock(mutex_);
    duckFrames_ = std::max(duckFrames_, frames);
}

Mixer::LoopbackStats Mixer::TakeLoopbackStats()
{
    std::lock_guard lock(mutex_);
    auto s = stats_;
    stats_ = {};
    statsFresh_ = true;
    return s;
}

bool Mixer::LoopbackEnabled() const
{
    std::lock_guard lock(mutex_);
    return loopback_;
}

void Mixer::PushLoopback(const float* samples, size_t count)
{
    std::lock_guard lock(mutex_);
    if (!loopback_) return;
    loopbackQueue_.insert(loopbackQueue_.end(), samples, samples + count);
    while (loopbackQueue_.size() > kMaxLoopbackQueue) {
        loopbackQueue_.pop_front();
        stats_.dropped++;
    }
    stats_.maxDepth = std::max(stats_.maxDepth, loopbackQueue_.size());
}

void Mixer::StartSamples(const std::string& key, int sampleRate, float volume, std::vector<std::vector<float>> channels,
                         std::function<void()> done)
{
    Samples s;
    s.volume = volume;
    s.done = std::move(done);
    if (!channels.empty() && sampleRate > 0) {
        size_t inFrames = channels[0].size();
        double ratio = 48000.0 / sampleRate;
        size_t outFrames = static_cast<size_t>(inFrames * ratio);
        s.stereo.resize(outFrames * 2);
        for (size_t i = 0; i < outFrames; i++) {
            double src = i / ratio;
            size_t i0 = static_cast<size_t>(src);
            size_t i1 = std::min(i0 + 1, inFrames - 1);
            float frac = static_cast<float>(src - i0);
            for (size_t c = 0; c < 2; c++) {
                auto& ch = channels[std::min(c, channels.size() - 1)];
                s.stereo[2 * i + c] = (ch[i0] * (1 - frac) + ch[i1] * frac) * 32767.0f;
            }
        }
    }
    std::function<void()> replaced;
    {
        std::lock_guard lock(mutex_);
        auto it = samples_.find(key);
        if (it != samples_.end()) replaced = std::move(it->second.done);
        samples_[key] = std::move(s);
    }
    if (replaced) replaced();
}

void Mixer::StopSamples(const std::string& key)
{
    std::function<void()> done;
    {
        std::lock_guard lock(mutex_);
        auto it = samples_.find(key);
        if (it == samples_.end()) return;
        done = std::move(it->second.done);
        samples_.erase(it);
    }
    if (done) done();
}

void Mixer::StopSamplesWithPrefix(const std::string& prefix)
{
    std::vector<std::function<void()>> done;
    {
        std::lock_guard lock(mutex_);
        for (auto it = samples_.begin(); it != samples_.end();) {
            if (it->first.rfind(prefix, 0) == 0) {
                done.push_back(std::move(it->second.done));
                it = samples_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& d : done)
        if (d) d();
}

bool Mixer::HasSamples() const
{
    std::lock_guard lock(mutex_);
    return !samples_.empty();
}

bool Mixer::AddSource(Source* source)
{
    std::lock_guard lock(mutex_);
    if (std::find(sources_.begin(), sources_.end(), source) == sources_.end()) sources_.push_back(source);
    return true;
}

void Mixer::RemoveSource(Source* source)
{
    std::lock_guard lock(mutex_);
    sources_.erase(std::remove(sources_.begin(), sources_.end(), source), sources_.end());
}

// The mic test's loopback is added to the playout after libwebrtc took its echo reference (CaptureAdm). In the
// reference the echo canceller took the test's own voice for echo of the speaker and suppressed the microphone while
// it was playing, which pulsed
// The mic test's loopback is added to the playout after libwebrtc took its echo reference (CaptureAdm::NeedMorePlayData).
// In the reference the echo canceller took the test's own voice for echo from the speaker and suppressed the
// microphone while it played, which pulsed
void Mixer::MixLoopbackInto(int16_t* data, size_t frames, size_t channels, uint32_t rate)
{
    if (channels == 0 || frames * 100 != rate) return; // 10 ms of the playout device's rate
    std::lock_guard lock(mutex_);
    if (!loopback_) return;
    float mix[kFrames] = {};
    bool any = false;
    // start once 30 ms are queued, so the capture's bursts do not leave gaps; buffer again after running dry
    if (!loopbackPrimed_ && loopbackQueue_.size() >= kLoopbackPrime) loopbackPrimed_ = true;
    if (loopbackPrimed_ && loopbackQueue_.size() < kFrames) {
        loopbackPrimed_ = false;
        stats_.underruns++;
    }
    if (statsFresh_) stats_.minDepth = loopbackQueue_.size(), statsFresh_ = false;
    else stats_.minDepth = std::min(stats_.minDepth, loopbackQueue_.size());
    if (loopbackPrimed_) {
        float target = duckFrames_ > 0 ? 0.f : 1.f;
        if (duckFrames_ > 0) duckFrames_--;
        for (size_t i = 0; i < kFrames; i++) {
            float s = loopbackQueue_.front();
            loopbackQueue_.pop_front();
            float g = duckGain_ + (target - duckGain_) * (i + 1) / kFrames; // a 10 ms ramp to the target
            mix[i] = s * g;
            any = true;
        }
        duckGain_ = target;
    }

    if (!any) return;
    // the device may play at another rate than the 48 kHz of the mixer
    float resampled[kFrames * 4];
    const float* in = mix;
    if (frames != kFrames) {
        if (frames > std::size(resampled)) return;
        if (!loopbackResampler_ || loopbackResamplerFrames_ != frames) {
            loopbackResampler_ = std::make_unique<webrtc::PushResampler<float>>(kFrames, frames, 1);
            loopbackResamplerFrames_ = frames;
        }
        loopbackResampler_->Resample(webrtc::MonoView<const float>(mix, kFrames), webrtc::MonoView<float>(resampled, frames));
        in = resampled;
    }
    for (size_t i = 0; i < frames; i++) {
        float v = in[i] * outputGain_;
        for (size_t c = 0; c < channels; c++)
            data[i * channels + c] = static_cast<int16_t>(std::clamp(data[i * channels + c] + v, -32768.0f, 32767.0f));
    }
}

void Mixer::Mix(size_t channels, webrtc::AudioFrame* frame)
{
    float mix[kFrames * 2] = {};
    std::vector<std::function<void()>> finished;
    {
        std::lock_guard lock(mutex_);
        webrtc::AudioFrame in;
        for (auto* source : sources_) {
            auto info = source->GetAudioFrameWithInfo(48000, &in);
            if (info != Source::AudioFrameInfo::kNormal || in.samples_per_channel() != kFrames) continue;
            SourceGain g;
            if (auto it = gains_.find(static_cast<uint32_t>(source->Ssrc())); it != gains_.end()) g = it->second;
            if (g.muted) continue; // still pulled above, so the jitter buffer keeps draining
            const int16_t* d = in.data();
            size_t ch = in.num_channels();
            for (size_t i = 0; i < kFrames; i++) {
                float l = d[i * ch];
                float r = ch > 1 ? d[i * ch + 1] : l;
                mix[2 * i] += l * g.left;
                mix[2 * i + 1] += r * g.right;
            }
        }

        for (auto it = samples_.begin(); it != samples_.end();) {
            auto& s = it->second;
            size_t n = std::min(kFrames * 2, s.stereo.size() - s.pos);
            for (size_t i = 0; i < n; i++) mix[i] += s.stereo[s.pos + i] * s.volume;
            s.pos += n;
            if (s.pos >= s.stereo.size()) {
                finished.push_back(std::move(s.done));
                it = samples_.erase(it);
            } else {
                ++it;
            }
        }

        frame->UpdateFrame(0, nullptr, kFrames, 48000, webrtc::AudioFrame::kNormalSpeech, webrtc::AudioFrame::kVadUnknown,
                           channels);
        int16_t* out = frame->mutable_data();
        for (size_t i = 0; i < kFrames; i++) {
            for (size_t c = 0; c < channels; c++) {
                float v = channels == 1 ? (mix[2 * i] + mix[2 * i + 1]) * 0.5f : mix[2 * i + std::min<size_t>(c, 1)];
                v *= outputGain_;
                // soft limit instead of hard clipping when several people talk at once
                if (std::abs(v) > 24000) v = std::copysign(24000 + 8767 * std::tanh((std::abs(v) - 24000) / 8767), v);
                out[i * channels + c] = static_cast<int16_t>(std::clamp(v, -32768.0f, 32767.0f));
            }
        }
    }
    for (auto& f : finished)
        if (f) f();
}

} // namespace vv
