// RNNoise (BSD) as the noise canceller, in the slot Discord's JS knows as Krisp. libvesktop_rnnoise.so (scripts/build-denoise.sh)
// is loaded at runtime from the module's own directory, never from the system; without it noise cancellation reports an error
// through setVoiceProcessingErrorCallback and audio passes unchanged.
#pragma once

#include <cstddef>
#include <cstdint>

namespace vv {

class RnNoise {
public:
    static bool Available();
    RnNoise();
    ~RnNoise();
    RnNoise(const RnNoise&) = delete;
    RnNoise& operator=(const RnNoise&) = delete;

    // 480 samples (10 ms @ 48 kHz), int16 range floats, in place. Returns the speech probability
    float Process(float* samples);
    explicit operator bool() const { return state_ != nullptr; }

private:
    void* state_ = nullptr;
};

} // namespace vv
