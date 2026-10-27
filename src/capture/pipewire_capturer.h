// Screen / window capture from a portal PipeWire node. Frames arrive in shared memory
// (no DMA-BUF import), are converted to I420 and pushed into a FrameSource.
#pragma once

#include <api/scoped_refptr.h>
#include <api/video/i420_buffer.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "capture/portal.h"
#include "capture/screen_capturer.h"
#include "video/frame_source.h"

namespace vv {

class PipeWireCapturer : public ScreenCapturer {
public:
    // `ended(reason)` runs once when the stream stops on its own (session closed, node removed, error)
    PipeWireCapturer(PickedSource source, webrtc::scoped_refptr<FrameSource> sink, std::function<void(const std::string&)> ended);
    ~PipeWireCapturer() override;
    bool Start();
    // Replaces the callback; runs it right away when the capture already ended
    void SetEnded(std::function<void(const std::string&)> ended) override;
    uint64_t Frames() const override { return frames_; }
    // The application of a picked window, when the compositor names it: KWin calls a window's stream
    // "kwin-screencast-<desktop file name>" (eg. "org.mozilla.firefox", "steam_app_4000"). "" if unknown
    std::string SourceAppId() const;
    // The app of the most recently started capture that named one (the capture may already have moved to a
    // connection when JS asks)
    static std::string LastAppId();

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    std::atomic<uint64_t> frames_{0};
};

} // namespace vv
