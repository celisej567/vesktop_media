// A running screen / window capture that feeds a FrameSource: PipeWireCapturer (Linux, a portal pick) or
// DesktopCaptureWin (Windows, a screen or window id from the Go Live picker)
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace vv {

class ScreenCapturer {
public:
    virtual ~ScreenCapturer() = default;
    // `ended(reason)` runs once when the capture stops on its own (window closed, screen gone, error); right away
    // when it already ended
    virtual void SetEnded(std::function<void(const std::string&)> ended) = 0;
    virtual uint64_t Frames() const = 0;
};

} // namespace vv
