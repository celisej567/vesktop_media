// DeepFilterNet (MIT / Apache-2.0) as a noise canceller, one of the models behind Discord's noise cancellation flag
// (Engine::SetNoiseModel). libvesktop_deepfilter.so (scripts/build-denoise.sh) is loaded at runtime from the module's
// own directory, never from the system, and carries its model (DeepFilterNet3).
// Creating a state loads the model (about 0.1 s), so it happens on a thread of its own: the audio passes unchanged
// until the state is ready. A processed frame costs about 0.2 ms.
#pragma once

#include <atomic>
#include <cstddef>
#include <thread>

namespace vv {

class DeepFilter {
public:
    // libvesktop_deepfilter.so loads from the module's directory and exports the C API
    static bool Available();
    // A state could not be created (the model did not load): not offered any more
    static bool Failed();

    // `onFailed` runs on the creating thread if the state cannot be created
    explicit DeepFilter(void (*onFailed)());
    ~DeepFilter();
    DeepFilter(const DeepFilter&) = delete;
    DeepFilter& operator=(const DeepFilter&) = delete;

    // 480 samples (10 ms @ 48 kHz), int16 range floats, in place. False while the state is not ready (unchanged)
    bool Process(float* samples);

private:
    std::thread creator_;
    std::atomic<void*> state_{nullptr};
};

} // namespace vv
