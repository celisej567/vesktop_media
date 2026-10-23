// Cameras: V4L2 through libwebrtc's video_capture module, or the fake "Strobe Video
// Source" with useFakeVideoCapture. One camera is selected (setVideoInputDevice) for calls; Go Live with a
// device (setGoLiveDevices) can use another one at the same time. A device captures while something uses it:
// a broadcasting connection or a JS preview sink on the device's guid. Each device has one frame source, shared
// by all its users.
//
// Everything that touches a device runs on the camera's own thread, never on the Call worker: opening a V4L2
// device takes 0.5 – 2 s. Enumerating devices opens them, which stalls or
// kills a running capture (measured: frames stop, each enumeration takes 3 – 7 s), so the list is cached and
// only rebuilt while nothing captures, when the set of /sys/class/video4linux nodes changed.
#pragma once

#include <api/scoped_refptr.h>
#include <modules/video_capture/video_capture.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "common/js.h"
#include "video/frame_source.h"

namespace vv {

class Camera {
public:
    static Camera& Get();

    void SetFake(bool fake) { fake_ = fake; }
    // Never touches hardware. System order (/dev/video*)
    json Devices() const;
    // False until the first enumeration finished (an empty list before that must not be reported)
    bool Ready() const { return ready_; }
    // Re-enumerates when the set of device nodes changed (async, no-op while capturing)
    void Refresh();
    // Enumerates now if needed, then `done(devices)` on the camera thread
    void GetDevices(std::function<void(const json&)> done);

    void SetDevice(const std::string& id); // guid, index, name or "disabled"
    std::string DeviceGuid() const;        // the camera in use (selected, or the first one)
    // guid of a device given by guid, index or name; "" if unknown
    std::string Resolve(const std::string& id) const;
    // The frame source of a device ("" = the selected camera); null for none / "disabled"
    webrtc::scoped_refptr<FrameSource> Source(const std::string& guid = "");

    // Vesktop extension: what a device can capture and per-device capture options
    // {formats: [{width, height, frameRate}], autofocus: bool | null (no such control)}
    json Capabilities(const std::string& id) const;
    // {width, height, frameRate, autofocus}; 0 / missing = whatever the users ask for; autofocus null = leave it
    void SetDeviceOptions(const std::string& id, const json& options);

    // Users: a broadcasting connection (key = its address + role) or the preview sink. guid "" = the selected camera
    void Acquire(const void* user, int width, int height, int fps, const std::string& guid = "");
    void Release(const void* user);
    void OnPreviewSinkChanged(const std::string& streamId, bool attached);

private:
    Camera();
    class Sink;
    struct Request {
        int width, height, fps;
        std::string guid; // "" = the selected camera
    };
    struct Format {
        int width, height, fps;
    };
    struct Entry {
        std::string name, guid;
        std::string path;           // /dev/videoN
        std::vector<Format> formats; // largest first
        bool hasAutofocus = false;
    };
    struct DeviceOptions {
        int width = 0, height = 0, fps = 0;
        int autofocus = -1; // -1 leave as is
    };
    struct Capture {
        int width = 0, height = 0, fps = 0;
        webrtc::scoped_refptr<webrtc::VideoCaptureModule> module;
        std::unique_ptr<Sink> sink;
        std::thread fakeThread;
        std::shared_ptr<std::atomic<bool>> fakeRunning;
    };

    void Post(std::function<void()> task);
    void Loop();
    // camera thread
    void Enumerate();
    void Apply();
    bool Start(const std::string& guid, Capture& c);
    void ApplyAutofocus(const std::string& guid);
    void Stop(const std::string& guid, Capture& c);
    std::string EffectiveGuidLocked() const; // mutex_ held
    std::string ResolveLocked(const std::string& id) const;
    webrtc::scoped_refptr<FrameSource> SourceLocked(const std::string& guid);

    std::atomic<bool> fake_{false};
    std::atomic<bool> ready_{false};
    std::atomic<bool> capturing_{false};

    mutable std::mutex mutex_; // state shared with the other threads
    std::vector<Entry> devices_;
    std::string selected_;      // "" = the first camera, "disabled" = none
    std::string effective_;     // what is selected / would be used
    std::map<const void*, Request> users_;
    std::map<std::string, bool> previewAttached_;                     // by guid
    std::map<std::string, webrtc::scoped_refptr<FrameSource>> sources_; // by guid, kept for the process lifetime
    std::map<std::string, DeviceOptions> options_;                      // by guid

    std::mutex queueMutex_;
    std::condition_variable queueCv_;
    std::deque<std::function<void()>> queue_;
    bool quit_ = false;
    std::thread thread_;

    // camera thread only
    std::string signature_;
    std::map<std::string, Capture> captures_; // running, by guid
};

} // namespace vv
