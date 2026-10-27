// The operating system's side of the cameras (capture/camera.cc does everything else): listing devices and their
// modes, noticing that the set changed, the autofocus control. V4L2 on Linux (camera_v4l2.cc), DirectShow through
// libwebrtc on Windows (camera_win.cc)
#pragma once

#include <array>
#include <string>
#include <vector>

namespace vv::camera_devices {

struct DeviceEntry {
    std::string name;
    std::string guid; // what libwebrtc's VideoCaptureFactory::Create opens
    std::string path; // the node (/dev/videoN) on Linux, "" on Windows
    std::vector<std::array<int, 3>> formats; // width, height, fps
};

// Every camera that can capture, with its modes, in system order; without sending requests to the cameras
std::vector<DeviceEntry> List();
// Changes when cameras come or go (cheap, opens no device)
std::string Signature();
// Whether a device has an autofocus control, and setting it. Asked only when somebody wants to know
bool HasAutofocus(const DeviceEntry& device);
bool SetAutofocus(const DeviceEntry& device, bool on);

} // namespace vv::camera_devices
