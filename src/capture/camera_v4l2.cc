// Cameras on Linux: V4L2 nodes, listed with queries only
#include "capture/camera_devices.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace vv::camera_devices {

namespace {

// Whether a node has an autofocus control. Asked only when somebody wants to know (the camera options), never while
// listing: a control query is a request to the camera itself
bool NodeHasAutofocus(const std::string& path)
{
    int fd = path.empty() ? -1 : open(path.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0) return false;
    v4l2_queryctrl q{};
    q.id = V4L2_CID_FOCUS_AUTO;
    bool ok = ioctl(fd, VIDIOC_QUERYCTRL, &q) == 0 && !(q.flags & V4L2_CTRL_FLAG_DISABLED);
    close(fd);
    return ok;
}

// The (width, height, frame rate) modes a node offers, from the driver's own descriptors: VIDIOC_ENUM_FMT,
// ENUM_FRAMESIZES and ENUM_FRAMEINTERVALS only. They answer from what the driver already knows and send nothing to
// the camera. (libwebrtc's DeviceInfoV4l2 finds the modes with a VIDIOC_TRY_FMT per format and size, about 200 of
// them: each one is a UVC probe request, and a camera that another program is streaming, an OBS source, reset on
// them: it dropped off the USB bus, came back and was probed again while Vesktop was open)
void EnumerateModes(int fd, std::vector<std::array<int, 3>>& out)
{
    auto add = [&](int w, int h, int fps) {
        std::array<int, 3> m{w, h, std::max(1, fps)};
        if (w > 0 && h > 0 && std::find(out.begin(), out.end(), m) == out.end()) out.push_back(m);
    };
    for (uint32_t fi = 0;; fi++) {
        v4l2_fmtdesc fmt{};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.index = fi;
        if (ioctl(fd, VIDIOC_ENUM_FMT, &fmt) != 0) break;
        for (uint32_t si = 0;; si++) {
            v4l2_frmsizeenum size{};
            size.pixel_format = fmt.pixelformat;
            size.index = si;
            if (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &size) != 0) break;
            std::vector<std::pair<int, int>> sizes;
            if (size.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
                sizes.emplace_back(size.discrete.width, size.discrete.height);
            } else {
                // a range (v4l2loopback): the usual sizes inside it, and its largest
                auto& r = size.stepwise;
                static const int kUsual[][2] = {{160, 120}, {320, 180}, {320, 240}, {640, 360}, {640, 480},
                                                {848, 480}, {960, 540}, {1280, 720}, {1600, 900}, {1920, 1080},
                                                {2560, 1440}, {3840, 2160}};
                for (auto& u : kUsual)
                    if (u[0] >= static_cast<int>(r.min_width) && u[0] <= static_cast<int>(r.max_width) &&
                        u[1] >= static_cast<int>(r.min_height) && u[1] <= static_cast<int>(r.max_height))
                        sizes.emplace_back(u[0], u[1]);
                sizes.emplace_back(r.max_width, r.max_height);
            }
            for (auto& [w, h] : sizes) {
                bool any = false;
                for (uint32_t ii = 0;; ii++) {
                    v4l2_frmivalenum iv{};
                    iv.pixel_format = fmt.pixelformat;
                    iv.width = w;
                    iv.height = h;
                    iv.index = ii;
                    if (ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &iv) != 0) break;
                    if (iv.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
                        if (iv.discrete.numerator > 0) add(w, h, static_cast<int>(iv.discrete.denominator / iv.discrete.numerator));
                        any = true;
                    } else {
                        // a range of intervals: the fastest, and 30 when it is within
                        auto& r = iv.stepwise;
                        if (r.min.numerator > 0) {
                            int fastest = static_cast<int>(r.min.denominator / r.min.numerator);
                            add(w, h, fastest);
                            if (fastest > 30) add(w, h, 30);
                        }
                        any = true;
                        break;
                    }
                }
                if (!any) add(w, h, 30); // no intervals told
            }
            if (size.type != V4L2_FRMSIZE_TYPE_DISCRETE) break; // a range has just the one entry
        }
    }
}

} // namespace

std::vector<DeviceEntry> List()
{
    std::vector<DeviceEntry> out;
    // every /dev/videoN once, with queries only. A capture node is one whose device_caps say video capture (the
    // nodes next to it are metadata); its guid is the bus_info libwebrtc names it by, its name the card
    for (int i = 0; i < 64; i++) {
        std::string path = "/dev/video" + std::to_string(i);
        int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        v4l2_capability cap{};
        bool capture = ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0 &&
                       (((cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities) & V4L2_CAP_VIDEO_CAPTURE);
        if (capture) {
            std::string guid = reinterpret_cast<const char*>(cap.bus_info);
            bool seen = false;
            for (auto& d : out) seen |= d.guid == guid;
            if (!seen) {
                DeviceEntry e{reinterpret_cast<const char*>(cap.card), guid, path};
                EnumerateModes(fd, e.formats);
                if (!e.formats.empty()) {
                    std::sort(e.formats.begin(), e.formats.end(), [](auto& a, auto& b) {
                        return a[0] * a[1] != b[0] * b[1] ? a[0] * a[1] > b[0] * b[1] : a[2] > b[2];
                    });
                    out.push_back(std::move(e));
                }
            }
        }
        close(fd);
    }
    return out;
}

// The set of device nodes, without opening any of them. v4l2loopback nodes (OBS Virtual Camera) are there all the
// time but only have formats while a producer feeds them, so their state (sysfs `state`, `format`) counts too
std::string Signature()
{
    std::vector<std::string> nodes;
    std::error_code ec;
    for (auto& e : std::filesystem::directory_iterator("/sys/class/video4linux", ec)) {
        auto read = [&](const char* file) {
            std::ifstream in(e.path() / file);
            std::string s;
            std::getline(in, s);
            return s;
        };
        nodes.push_back(e.path().filename().string() + ":" + read("name") + ":" + read("state") + ":" + read("format"));
    }
    std::sort(nodes.begin(), nodes.end()); // the directory's order is not promised
    std::string sig;
    for (auto& n : nodes) sig += n + "\n";
    return sig;
}

bool HasAutofocus(const DeviceEntry& device)
{
    return NodeHasAutofocus(device.path);
}

bool SetAutofocus(const DeviceEntry& device, bool on)
{
    int fd = device.path.empty() ? -1 : open(device.path.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0) return false;
    v4l2_control c{};
    c.id = V4L2_CID_FOCUS_AUTO;
    c.value = on ? 1 : 0;
    bool ok = ioctl(fd, VIDIOC_S_CTRL, &c) == 0;
    close(fd);
    return ok;
}

} // namespace vv::camera_devices
