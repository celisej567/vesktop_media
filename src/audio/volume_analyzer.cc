#include "audio/volume_analyzer.h"

#include <algorithm>
#include <cmath>

namespace vv {

namespace {

void Fft(std::vector<std::complex<float>>& a)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        float ang = -2.0f * static_cast<float>(M_PI) / static_cast<float>(len);
        std::complex<float> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1);
            for (size_t j = 0; j < len / 2; j++) {
                auto u = a[i + j], v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

} // namespace

VolumeAnalyzer::VolumeAnalyzer() : fft_(kSize)
{
    for (size_t n = 0; n < kSize; n++) {
        double x = 2.0 * M_PI * static_cast<double>(n) / static_cast<double>(kSize);
        window_[n] = static_cast<float>(0.42 - 0.5 * std::cos(x) + 0.08 * std::cos(2 * x));
    }
}

void VolumeAnalyzer::Reset()
{
    history_.fill(0);
    smoothed_.fill(0);
    pos_ = 0;
}

float VolumeAnalyzer::Process(const float* samples, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        history_[pos_] = samples[i];
        pos_ = (pos_ + 1) % kSize;
    }
    for (size_t n = 0; n < kSize; n++) fft_[n] = history_[(pos_ + n) % kSize] * window_[n];
    Fft(fft_);

    float peak = 0;
    for (size_t k = 0; k < kSize / 2; k++) {
        float mag = std::abs(fft_[k]) / static_cast<float>(kSize);
        // smoothing factor 0.1 towards the previous value
        smoothed_[k] = smoothed_[k] * 0.1f + mag * 0.9f;
        peak = std::max(peak, smoothed_[k]);
    }
    return 20.0f * std::log10(std::max(peak, 1e-5f));
}

} // namespace vv
