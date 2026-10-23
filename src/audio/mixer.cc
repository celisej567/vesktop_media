#include "audio/mixer.h"

#include <api/audio/audio_frame.h>

#include <algorithm>
#include <cmath>

namespace vv {

namespace {
constexpr size_t kFrames = 480; // 10 ms @ 48 kHz
constexpr size_t kMaxLoopbackQueue = 48000 / 5; // 200 ms
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
    loopback_ = enabled;
    loopbackQueue_.clear();
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
    while (loopbackQueue_.size() > kMaxLoopbackQueue) loopbackQueue_.pop_front();
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

        if (loopback_ && loopbackQueue_.size() >= kFrames) {
            for (size_t i = 0; i < kFrames; i++) {
                float s = loopbackQueue_.front();
                loopbackQueue_.pop_front();
                mix[2 * i] += s;
                mix[2 * i + 1] += s;
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
