// xdg-desktop-portal ScreenCast (Linux / Wayland): the system picker. There is one global portal session; presenting the picker again (or releasing it) closes the
// previous one, which ends a capture that uses it.
#pragma once

#include <api/scoped_refptr.h>

#include "capture/gio_api.h"
#include "common/js.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace vv {

class PipeWireCapturer;
class FrameSource;

struct PickedSource {
    int fd = -1; // PipeWire remote (owned by the portal session; dup() it to use)
    uint32_t nodeId = 0;
    int width = 0, height = 0;
    uint32_t sourceType = 0; // 1 monitor, 2 window
    std::string appId;       // a window's application (desktop file name), when the compositor tells
};

class ScreenCastPortal {
public:
    static ScreenCastPortal& Get();
    ~ScreenCastPortal();

    enum class Outcome { Picked, Cancelled, Error };
    // style "window" = windows, anything else = screens. `done` runs on the portal thread
    void Present(const std::string& style, std::function<void(Outcome, const std::string& error)> done);
    // The last picked source, if its session is still open. fd is a dup the caller owns
    bool TakePicked(PickedSource& out);
    // The capture that was started right after the pick (Discord only starts the stream after its quality
    // dialog; the capture feeds the preview meanwhile). Hands over the running capturer and its frame source, once
    bool TakeCapture(std::unique_ptr<PipeWireCapturer>& capturer, webrtc::scoped_refptr<FrameSource>& source);
    // What was picked last: {sourceType: "screen" | "window", appId} (Vesktop extension)
    json PickedInfo();
    // JS releases the picker's stream: drops a pick nobody took; a capture a connection already took goes on
    void Release();
    // The capture that used the session is gone: closes it
    void CloseSession();

private:
    void Run(uint32_t types, std::function<void(Outcome, const std::string&)> done);
    void CloseSessionLocked();
    GDBusConnection* Bus(GError** err);

    std::mutex mutex_;
    std::thread thread_;
    bool busy_ = false;
    std::string session_;
    PickedSource picked_;
    std::unique_ptr<PipeWireCapturer> capturer_;
    webrtc::scoped_refptr<FrameSource> source_;
    unsigned counter_ = 0;
    std::mutex busMutex_;
    GDBusConnection* bus_ = nullptr;
};

} // namespace vv
