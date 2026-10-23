#pragma once

#include <cstddef>

namespace vv {

// Everything inside the engine runs at 48 kHz in 10 ms frames
constexpr int kSampleRate = 48000;
constexpr size_t kFrameSamples = 480;      // 10 ms per channel
constexpr size_t kOpusFrameSamples = 960;  // 20 ms, pacsize from audioEncoder

} // namespace vv
