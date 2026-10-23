#include "capture/camera.h"

#include <api/make_ref_counted.h>
#include <api/video/i420_buffer.h>
#include <modules/video_capture/video_capture_factory.h>
#include <rtc_base/time_utils.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <memory>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "common/log.h"

namespace vv {

namespace {

constexpr const char* kFakeName = "Strobe Video Source";
constexpr const char* kFakeName2 = "Strobe Video Source 2"; // a second one, to switch between (the tests)

struct DeviceEntry {
    std::string name, guid, path;
    std::vector<std::array<int, 3>> formats; // width, height, fps
    bool hasAutofocus = false;
};

// Whether a node has an autofocus control. Asked only when somebody wants to know (the camera options), never while
// listing: a control query is a request to the camera itself
bool HasAutofocus(const std::string& path)
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

std::vector<DeviceEntry> ListDevices(bool fake)
{
    std::vector<DeviceEntry> out;
    if (fake) {
        for (const char* name : {kFakeName, kFakeName2})
            out.push_back({name, name, "", {{1920, 1080, 30}, {1280, 720, 60}, {1280, 720, 30}, {640, 360, 30}}, false});
        return out;
    }
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

// Moving colour bars, so a frozen stream is visible
webrtc::scoped_refptr<webrtc::I420Buffer> FakeFrame(int width, int height, int n)
{
    auto buf = webrtc::I420Buffer::Create(width, height);
    static const uint8_t bars[8][3] = {{235, 128, 128}, {210, 16, 146}, {170, 166, 16}, {145, 54, 34},
                                       {106, 202, 222}, {81, 90, 240}, {41, 240, 110}, {16, 128, 128}};
    int shift = (n * 4) % width;
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++) buf->MutableDataY()[y * buf->StrideY() + x] = bars[((x + shift) * 8 / width) % 8][0];
    for (int y = 0; y < height / 2; y++)
        for (int x = 0; x < width / 2; x++) {
            auto& c = bars[((x * 2 + shift) * 8 / width) % 8];
            buf->MutableDataU()[y * buf->StrideU() + x] = c[1];
            buf->MutableDataV()[y * buf->StrideV() + x] = c[2];
        }
    // a white square bouncing up and down
    int sq = height / 6, sx = width / 2 - sq / 2, sy = static_cast<int>((height - sq) * (0.5 + 0.5 * std::sin(n / 10.0)));
    for (int y = sy; y < sy + sq; y++) memset(buf->MutableDataY() + y * buf->StrideY() + sx, 235, sq);
    return buf;
}

} // namespace

class Camera::Sink : public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
public:
    explicit Sink(FrameSource* source) : source_(source) {}
    void OnFrame(const webrtc::VideoFrame& frame) override { source_->PushFrame(frame); }

private:
    FrameSource* source_;
};

Camera& Camera::Get()
{
    static auto* camera = new Camera();
    return *camera;
}

Camera::Camera()
{
    thread_ = std::thread([this] { Loop(); });
}

void Camera::Post(std::function<void()> task)
{
    {
        std::lock_guard lock(queueMutex_);
        queue_.push_back(std::move(task));
    }
    queueCv_.notify_one();
}

void Camera::Loop()
{
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock lock(queueMutex_);
            queueCv_.wait(lock, [this] { return quit_ || !queue_.empty(); });
            if (quit_) return;
            task = std::move(queue_.front());
            queue_.pop_front();
        }
        task();
    }
}

json Camera::Devices() const
{
    std::lock_guard lock(mutex_);
    json list = json::array();
    int index = 0;
    for (auto& d : devices_)
        list.push_back({{"name", d.name}, {"guid", d.guid}, {"index", index++}, {"facing", "unknown"}});
    return list;
}

std::string Camera::DeviceGuid() const
{
    std::lock_guard lock(mutex_);
    return effective_;
}

std::string Camera::Resolve(const std::string& id) const
{
    std::lock_guard lock(mutex_);
    return ResolveLocked(id);
}

std::string Camera::ResolveLocked(const std::string& id) const
{
    if (id.empty() || id == "disabled") return id;
    if (id == "default") return effective_; // Discord's "default" camera entry
    for (auto& d : devices_)
        if (d.guid == id) return id;
    // an index (without set_video_device_by_id) or a name also select a device
    char* end = nullptr;
    long index = strtol(id.c_str(), &end, 10);
    if (end && *end == 0 && index >= 0 && index < static_cast<long>(devices_.size())) return devices_[index].guid;
    for (auto& d : devices_)
        if (d.name == id) return d.guid;
    return id; // not enumerated (yet): taken as a guid
}

json Camera::Capabilities(const std::string& id)
{
    std::lock_guard lock(mutex_);
    std::string guid = ResolveLocked(id.empty() ? effective_ : id);
    json formats = json::array();
    json autofocus = nullptr;
    for (auto& d : devices_) {
        if (d.guid != guid) continue;
        if (!d.autofocusKnown) {
            d.hasAutofocus = HasAutofocus(d.path);
            d.autofocusKnown = true;
        }
        for (auto& f : d.formats) formats.push_back({{"width", f.width}, {"height", f.height}, {"frameRate", f.fps}});
        if (d.hasAutofocus) autofocus = true;
    }
    json options = json::object();
    if (auto it = options_.find(guid); it != options_.end()) {
        auto& o = it->second;
        options = {{"width", o.width}, {"height", o.height}, {"frameRate", o.fps}};
        options["autofocus"] = o.autofocus < 0 ? json(nullptr) : json(o.autofocus == 1);
    }
    return {{"guid", guid}, {"formats", formats}, {"autofocus", autofocus}, {"options", options}};
}

void Camera::SetDeviceOptions(const std::string& id, const json& o)
{
    std::string guid;
    {
        std::lock_guard lock(mutex_);
        guid = ResolveLocked(id.empty() ? effective_ : id);
        if (guid.empty() || guid == "disabled") return;
        DeviceOptions d;
        d.width = std::max(0, vv::Get(o, "width", 0));
        d.height = std::max(0, vv::Get(o, "height", 0));
        d.fps = std::max(0, vv::Get(o, "frameRate", 0));
        if (o.contains("autofocus") && o["autofocus"].is_boolean()) d.autofocus = o["autofocus"].get<bool>() ? 1 : 0;
        VV_LOGI("camera", "options for %s: %dx%d @ %d, autofocus %d", guid.c_str(), d.width, d.height, d.fps, d.autofocus);
        options_[guid] = d;
    }
    Post([this, guid] {
        Apply();
        // on a capture that keeps running; a camera that is not captured by us is left alone (it may be OBS's)
        if (captures_.count(guid)) ApplyAutofocus(guid);
    });
}

void Camera::ApplyAutofocus(const std::string& guid)
{
    int value;
    std::string path;
    {
        std::lock_guard lock(mutex_);
        auto it = options_.find(guid);
        if (it == options_.end() || it->second.autofocus < 0) return;
        value = it->second.autofocus;
        for (auto& d : devices_)
            if (d.guid == guid) path = d.path;
    }
    int fd = path.empty() ? -1 : open(path.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0) return;
    v4l2_control c{};
    c.id = V4L2_CID_FOCUS_AUTO;
    c.value = value;
    if (ioctl(fd, VIDIOC_S_CTRL, &c) != 0) VV_LOGW("camera", "%s: cannot set autofocus %d", guid.c_str(), value);
    else VV_LOGI("camera", "%s: autofocus %s", guid.c_str(), value ? "on" : "off");
    close(fd);
}

webrtc::scoped_refptr<FrameSource> Camera::Source(const std::string& guid)
{
    std::lock_guard lock(mutex_);
    return SourceLocked(guid.empty() ? effective_ : guid);
}

webrtc::scoped_refptr<FrameSource> Camera::SourceLocked(const std::string& guid)
{
    if (guid.empty() || guid == "disabled") return nullptr;
    auto& s = sources_[guid];
    if (!s) {
        s = webrtc::make_ref_counted<FrameSource>(false, "camera");
        s->SetPreviewStreamId(guid); // the camera preview sink is the device's guid
    }
    return s;
}

std::string Camera::EffectiveGuidLocked() const
{
    if (selected_ == "disabled") return "disabled";
    if (!selected_.empty()) return selected_;
    return devices_.empty() ? "" : devices_[0].guid;
}

// The set of device nodes, without opening any of them. v4l2loopback nodes (OBS Virtual Camera) are there all the
// time but only have formats while a producer feeds them, so their state (sysfs `state`, `format`) counts too
static std::string NodeSignature()
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

void Camera::Enumerate()
{
    auto list = ListDevices(fake_);
    {
        std::lock_guard lock(mutex_);
        devices_.clear();
        for (auto& d : list) {
            Entry e{d.name, d.guid, d.path, {}, false, false};
            for (auto& f : d.formats) e.formats.push_back({f[0], f[1], f[2]});
            devices_.push_back(std::move(e));
        }
        effective_ = EffectiveGuidLocked();
    }
    ready_ = true;
}

void Camera::Refresh()
{
    if (capturing_) return;
    Post([this] {
        {
            std::lock_guard lock(mutex_);
            if (capturing_ || !users_.empty()) return;
        }
        if (fake_) {
            if (!ready_) Enumerate();
            return;
        }
        auto sig = NodeSignature();
        if (ready_ && sig == signature_) {
            pendingSignature_.clear();
            return;
        }
        auto now = std::chrono::steady_clock::now();
        if (ready_) {
            // Enumerating opens every camera. A camera that is plugging in, resetting or being taken by another
            // program (OBS) makes its nodes come and go every few seconds, and opening it meanwhile (power up,
            // power down) kept it doing that: wait until the nodes stay as they are for one more poll, and not
            // more often than every 5 s
            if (sig != pendingSignature_) {
                if (pendingSignature_.empty()) VV_LOGI("camera", "the camera nodes changed, waiting for them to settle");
                pendingSignature_ = sig;
                return;
            }
            if (now - lastEnumerated_ < std::chrono::seconds(5)) return;
        }
        pendingSignature_.clear();
        signature_ = sig;
        auto t0 = now;
        Enumerate();
        lastEnumerated_ = std::chrono::steady_clock::now();
        VV_LOGI("camera", "enumerated cameras in %lld ms",
                static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(lastEnumerated_ - t0).count()));
    });
}

void Camera::GetDevices(std::function<void(const json&)> done)
{
    Post([this, done = std::move(done)] {
        if (!ready_ && !capturing_) {
            signature_ = fake_ ? "" : NodeSignature();
            Enumerate();
        }
        done(Devices());
    });
}

void Camera::SetDevice(const std::string& id)
{
    {
        std::lock_guard lock(mutex_);
        std::string guid = ResolveLocked(id);
        if (guid == selected_) return;
        VV_LOGI("camera", "video input device -> %s", guid.c_str());
        selected_ = guid;
        effective_ = EffectiveGuidLocked();
    }
    Post([this] { Apply(); });
    // the calls that send the selected camera take its frame source from it when their video send is set up:
    // without being told they went on sending the old camera until something else changed their video
    if (selectionChanged_) selectionChanged_();
}

void Camera::Acquire(const void* user, int width, int height, int fps, const std::string& guid)
{
    {
        std::lock_guard lock(mutex_);
        Request r{width, height, fps, guid};
        auto it = users_.find(user);
        if (it != users_.end() && it->second.width == r.width && it->second.height == r.height && it->second.fps == r.fps &&
            it->second.guid == r.guid)
            return;
        users_[user] = r;
    }
    Post([this] { Apply(); });
}

void Camera::Release(const void* user)
{
    {
        std::lock_guard lock(mutex_);
        if (!users_.erase(user)) return;
    }
    Post([this] { Apply(); });
}

void Camera::OnPreviewSinkChanged(const std::string& streamId, bool attached)
{
    {
        std::lock_guard lock(mutex_);
        // only device guids (call / Go Live previews of other ids are not the camera's)
        bool known = streamId == effective_ || sources_.count(streamId);
        for (auto& d : devices_) known |= d.guid == streamId;
        if (!known) return;
        previewAttached_[streamId] = attached;
    }
    Post([this] { Apply(); });
}

// camera thread: bring the captures in line with who wants them
void Camera::Apply()
{
    struct Want {
        int width = 1280, height = 720, fps = 30;
    };
    std::map<std::string, Want> wanted;
    if (!ready_) {
        bool any;
        {
            std::lock_guard lock(mutex_);
            any = !users_.empty();
            for (auto& [g, on] : previewAttached_) any |= on;
        }
        if (any) Enumerate();
    }
    {
        std::lock_guard lock(mutex_);
        for (auto& [u, r] : users_) {
            std::string guid = r.guid.empty() ? effective_ : r.guid;
            if (guid.empty() || guid == "disabled") continue;
            // the largest request wins (default 1280x720 @ 30)
            auto [it, fresh] = wanted.try_emplace(guid);
            auto& w = it->second;
            if (r.width * r.height > 0 && (fresh || r.width * r.height >= w.width * w.height)) w.width = r.width, w.height = r.height;
            if (r.fps > 0) w.fps = fresh ? r.fps : std::max(w.fps, r.fps);
        }
        for (auto& [guid, on] : previewAttached_)
            if (on && guid != "disabled") wanted.try_emplace(guid);
        // the options chosen for a device (Vesktop) win over what the users ask for
        for (auto& [guid, w] : wanted) {
            auto it = options_.find(guid);
            if (it == options_.end()) continue;
            if (it->second.width > 0 && it->second.height > 0) w.width = it->second.width, w.height = it->second.height;
            if (it->second.fps > 0) w.fps = it->second.fps;
        }
    }
    for (auto it = captures_.begin(); it != captures_.end();) {
        auto w = wanted.find(it->first);
        if (w == wanted.end() || w->second.width != it->second.width || w->second.height != it->second.height ||
            w->second.fps != it->second.fps) {
            Stop(it->first, it->second);
            lastStopped_[it->first] = std::chrono::steady_clock::now();
            it = captures_.erase(it);
        } else {
            ++it;
        }
    }
    auto now = std::chrono::steady_clock::now();
    for (auto& [guid, w] : wanted) {
        if (captures_.count(guid)) continue;
        if (auto busy = busyUntil_.find(guid); busy != busyUntil_.end() && now < busy->second) continue;
        Capture c;
        c.width = w.width;
        c.height = w.height;
        c.fps = w.fps;
        if (Start(guid, c)) {
            busyUntil_.erase(guid);
            captures_.emplace(guid, std::move(c));
            ApplyAutofocus(guid);
        } else {
            busyUntil_[guid] = now + std::chrono::seconds(15);
        }
    }
    capturing_ = !captures_.empty();
}

bool Camera::Start(const std::string& guid, Capture& c)
{
    int width = c.width, height = c.height, fps = c.fps;
    webrtc::scoped_refptr<FrameSource> source;
    {
        std::lock_guard lock(mutex_);
        source = SourceLocked(guid);
    }

    if (fake_) {
        if (guid != kFakeName && guid != kFakeName2) {
            VV_LOGW("camera", "no camera %s", guid.c_str());
            return false;
        }
        c.fakeRunning = std::make_shared<std::atomic<bool>>(true);
        c.fakeThread = std::thread([source, running = c.fakeRunning, width, height, fps] {
            int n = 0;
            auto next = std::chrono::steady_clock::now();
            while (*running) {
                source->PushFrame(webrtc::VideoFrame::Builder()
                                      .set_video_frame_buffer(FakeFrame(width, height, n++))
                                      .set_timestamp_us(webrtc::TimeMicros())
                                      .build());
                next += std::chrono::microseconds(1'000'000 / fps);
                std::this_thread::sleep_until(next);
            }
        });
        VV_LOGI("camera", "fake camera started: %dx%d @ %d", width, height, fps);
        return true;
    }

    c.module = webrtc::VideoCaptureFactory::Create(guid.c_str());
    if (!c.module) {
        VV_LOGE("camera", "cannot open %s", guid.c_str());
        return false;
    }
    // the mode closest to the request, from the modes we listed (libwebrtc's own way of matching would probe the
    // device again with VIDIOC_TRY_FMT, see EnumerateModes)
    std::vector<Format> modes;
    bool justStopped = false;
    {
        std::lock_guard lock(mutex_);
        for (auto& d : devices_)
            if (d.guid == guid) modes = d.formats;
        auto stopped = lastStopped_.find(guid);
        justStopped = stopped != lastStopped_.end() && std::chrono::steady_clock::now() - stopped->second < std::chrono::seconds(3);
    }
    webrtc::VideoCaptureCapability capability;
    capability.width = width;
    capability.height = height;
    capability.maxFPS = fps;
    capability.videoType = webrtc::VideoType::kI420;
    {
        // smallest distance in size, then in frame rate (a rate under the request counts double)
        long best = -1;
        for (auto& m : modes) {
            long distance = (std::labs(static_cast<long>(m.width) - width) + std::labs(static_cast<long>(m.height) - height)) * 1000 +
                            (m.fps >= fps ? m.fps - fps : (fps - m.fps) * 2);
            if (best >= 0 && distance >= best) continue;
            best = distance;
            capability.width = m.width;
            capability.height = m.height;
            capability.maxFPS = m.fps;
        }
    }
    c.sink = std::make_unique<Sink>(source.get());
    c.module->RegisterCaptureDataCallback(c.sink.get());
    // A device that was just closed by us (another mode, another user) can still be busy for a moment. Otherwise a
    // failure is another program holding the camera (OBS): each request to such a camera is a probe of it, so one
    // is enough (Apply() does not try again for 15 s)
    bool started = false;
    for (int attempt = 0; attempt < (justStopped ? 3 : 1) && !started; attempt++) {
        if (attempt) {
            VV_LOGW("camera", "cannot start %s, retrying", guid.c_str());
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        started = c.module->StartCapture(capability) == 0;
    }
    if (!started) {
        VV_LOGE("camera", "cannot start %s (used by another program, or not ready); not trying again for 15 s", guid.c_str());
        c.module->DeRegisterCaptureDataCallback();
        c.module = nullptr;
        c.sink.reset();
        return false;
    }
    VV_LOGI("camera", "camera %s started: %dx%d @ %d (type %d)", guid.c_str(), capability.width, capability.height,
            capability.maxFPS, static_cast<int>(capability.videoType));
    return true;
}

void Camera::Stop(const std::string& guid, Capture& c)
{
    if (c.fakeRunning) {
        *c.fakeRunning = false;
        if (c.fakeThread.joinable()) c.fakeThread.join();
        VV_LOGI("camera", "fake camera stopped");
    }
    if (c.module) {
        c.module->StopCapture();
        c.module->DeRegisterCaptureDataCallback();
        c.module = nullptr;
        VV_LOGI("camera", "camera %s stopped", guid.c_str());
    }
    c.sink.reset();
}

} // namespace vv
