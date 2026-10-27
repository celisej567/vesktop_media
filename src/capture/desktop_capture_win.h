// Screens and windows on Windows, through libwebrtc's DesktopCapturer: DXGI desktop duplication for screens
// (GDI when that is not available), Windows.Graphics.Capture for windows (GDI before Windows 10 1903).
//
// Discord's Go Live picker asks the module for the sources (getScreenPreviews / getWindowPreviews, the
// screen_previews / window_previews features) and starts the stream with the picked id: "screen:<id>:0" or
// "window:<HWND>:0", which reaches setDesktopSource as type "screen" / "window" and the id
#pragma once

#include <api/scoped_refptr.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "capture/screen_capturer.h"
#include "common/js.h"
#include "video/frame_source.h"

namespace vv {

// [{id, name, url}] with a PNG thumbnail of at most width x height as a data URL, the shape of Electron's
// desktopCapturer sources that Discord's picker shows. Takes a while (one frame of each source): not on the JS thread
json DesktopPreviews(bool windows, int width, int height);

class DesktopCaptureWin : public ScreenCapturer {
public:
    enum class Kind { Screen, Window };
    DesktopCaptureWin(Kind kind, int64_t id, webrtc::scoped_refptr<FrameSource> sink, std::function<void(const std::string&)> ended);
    ~DesktopCaptureWin() override;
    // Opens the source and starts capturing; false when it cannot be captured
    bool Start();
    void SetEnded(std::function<void(const std::string&)> ended) override;
    uint64_t Frames() const override { return frames_; }

private:
    void Loop(std::shared_ptr<std::atomic<int>> started);
    void End(const std::string& reason);

    const Kind kind_;
    const int64_t id_;
    webrtc::scoped_refptr<FrameSource> sink_;
    std::mutex endedMutex_;
    std::function<void(const std::string&)> ended_;
    bool hasEnded_ = false;
    std::string endReason_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> frames_{0};
    std::thread thread_;
};

} // namespace vv
