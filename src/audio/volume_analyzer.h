// Level meter: 512 sample Blackman-windowed FFT, per bin magnitudes smoothed over time, level = peak bin in dBFS (floor -100).
#pragma once

#include <array>
#include <complex>
#include <cstddef>
#include <vector>

namespace vv {

class VolumeAnalyzer {
public:
    static constexpr size_t kSize = 512;
    VolumeAnalyzer();
    // Feeds samples and returns the current level in dBFS
    float Process(const float* samples, size_t count);
    void Reset();

private:
    std::array<float, kSize> window_{};
    std::array<float, kSize> history_{};
    std::array<float, kSize / 2> smoothed_{};
    std::vector<std::complex<float>> fft_;
    size_t pos_ = 0;
};

} // namespace vv
