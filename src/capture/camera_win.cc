// Cameras on Windows: DirectShow video input devices through libwebrtc's DeviceInfo (DeviceInfoDS). Listing reads
// the filters' registered capabilities and does not start the camera
#include "capture/camera_devices.h"

#include <modules/video_capture/video_capture_factory.h>

#include <algorithm>
#include <memory>

namespace vv::camera_devices {

namespace {

std::unique_ptr<webrtc::VideoCaptureModule::DeviceInfo> Info()
{
    return std::unique_ptr<webrtc::VideoCaptureModule::DeviceInfo>(webrtc::VideoCaptureFactory::CreateDeviceInfo());
}

} // namespace

std::vector<DeviceEntry> List()
{
    std::vector<DeviceEntry> out;
    auto info = Info();
    if (!info) return out;
    for (uint32_t i = 0; i < info->NumberOfDevices(); i++) {
        char name[256] = {}, guid[256] = {}, product[256] = {};
        if (info->GetDeviceName(i, name, sizeof(name), guid, sizeof(guid), product, sizeof(product)) != 0) continue;
        DeviceEntry e{name, guid, "", {}};
        int32_t n = info->NumberOfCapabilities(guid);
        for (int32_t c = 0; c < n; c++) {
            webrtc::VideoCaptureCapability cap;
            if (info->GetCapability(guid, c, cap) != 0) continue;
            std::array<int, 3> m{cap.width, cap.height, std::max(1, cap.maxFPS)};
            if (m[0] > 0 && m[1] > 0 && std::find(e.formats.begin(), e.formats.end(), m) == e.formats.end()) e.formats.push_back(m);
        }
        // a device without registered capabilities (some virtual cameras) still captures at 720p30
        if (e.formats.empty()) e.formats.push_back({1280, 720, 30});
        std::sort(e.formats.begin(), e.formats.end(), [](auto& a, auto& b) {
            return a[0] * a[1] != b[0] * b[1] ? a[0] * a[1] > b[0] * b[1] : a[2] > b[2];
        });
        out.push_back(std::move(e));
    }
    return out;
}

// The names and ids of the video input filters: enumerating the category does not open a device
std::string Signature()
{
    std::string sig;
    auto info = Info();
    if (!info) return sig;
    for (uint32_t i = 0; i < info->NumberOfDevices(); i++) {
        char name[256] = {}, guid[256] = {};
        if (info->GetDeviceName(i, name, sizeof(name), guid, sizeof(guid)) == 0) sig.append(name).append(":").append(guid).append("\n");
    }
    return sig;
}

// IAMCameraControl is not reached through libwebrtc; the option is not offered
bool HasAutofocus(const DeviceEntry&)
{
    return false;
}

bool SetAutofocus(const DeviceEntry&, bool)
{
    return false;
}

} // namespace vv::camera_devices
