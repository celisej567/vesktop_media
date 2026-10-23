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

struct DeviceEntry {
    std::string name, guid, path;
    std::vector<std::array<int, 3>> formats; // width, height, fps
    bool hasAutofocus = false;
};

// The /dev/videoN capture node of a libwebrtc device id (its V4L2 bus_info), "" if none
std::string DevicePath(const std::string& guid)
{
    for (int i = 0; i < 64; i++) {
        std::string path = "/dev/video" + std::to_string(i);
        int fd = open(path.c_str(), O_RDWR | O_NONBLOCK);
        if (fd < 0) continue;
        v4l2_capability cap{};
        bool match = ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0 &&
                     ((cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities) & V4L2_CAP_VIDEO_CAPTURE &&
                     (guid == reinterpret_cast<const char*>(cap.bus_info) || guid == reinterpret_cast<const char*>(cap.card));
        close(fd);
        if (match) return path;
    }
    return "";
}

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

std::vector<DeviceEntry> ListDevices(bool fake)
{
    std::vector<DeviceEntry> out;
    if (fake) {
        out.push_back({kFakeName, kFakeName, "", {{1920, 1080, 30}, {1280, 720, 60}, {1280, 720, 30}, {640, 360, 30}}, false});
        return out;
    }
    std::unique_ptr<webrtc::VideoCaptureModule::DeviceInfo> info(webrtc::VideoCaptureFactory::CreateDeviceInfo());
    if (!info) return out;
    for (uint32_t i = 0; i < info->NumberOfDevices(); i++) {
        char name[256] = {}, id[256] = {}, product[256] = {};
        if (info->GetDeviceName(i, name, sizeof(name), id, sizeof(id), product, sizeof(product)) != 0) continue;
        int count = info->NumberOfCapabilities(id);
        if (count <= 0) continue; // metadata nodes
        DeviceEntry e{name, id};
        for (int c = 0; c < count; c++) {
            webrtc::VideoCaptureCapability cap;
            if (info->GetCapability(id, c, cap) != 0 || cap.width <= 0 || cap.height <= 0) continue;
            std::array<int, 3> f{cap.width, cap.height, cap.maxFPS};
            if (std::find(e.formats.begin(), e.formats.end(), f) == e.formats.end()) e.formats.push_back(f);
        }
        std::sort(e.formats.begin(), e.formats.end(), [](auto& a, auto& b) {
            return a[0] * a[1] != b[0] * b[1] ? a[0] * a[1] > b[0] * b[1] : a[2] > b[2];
        });
        e.path = DevicePath(id);
        e.hasAutofocus = HasAutofocus(e.path);
        out.push_back(std::move(e));
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

json Camera::Capabilities(const std::string& id) const
{
    std::lock_guard lock(mutex_);
    std::string guid = ResolveLocked(id.empty() ? effective_ : id);
    json formats = json::array();
    json autofocus = nullptr;
    for (auto& d : devices_) {
        if (d.guid != guid) continue;
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
        ApplyAutofocus(guid); // also on a capture that keeps running
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

// The set of device nodes, without opening any of them
static std::string NodeSignature()
{
    std::string sig;
    std::error_code ec;
    for (auto& e : std::filesystem::directory_iterator("/sys/class/video4linux", ec)) {
        std::ifstream name(e.path() / "name");
        std::string n;
        std::getline(name, n);
        sig += e.path().filename().string() + ":" + n + "\n";
    }
    return sig;
}

void Camera::Enumerate()
{
    auto list = ListDevices(fake_);
    {
        std::lock_guard lock(mutex_);
        devices_.clear();
        for (auto& d : list) {
            Entry e{d.name, d.guid, d.path, {}, d.hasAutofocus};
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
        if (ready_ && sig == signature_) return;
        signature_ = sig;
        auto t0 = std::chrono::steady_clock::now();
        Enumerate();
        VV_LOGI("camera", "enumerated cameras in %lld ms",
                static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count()));
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
            it = captures_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto& [guid, w] : wanted) {
        if (captures_.count(guid)) continue;
        Capture c;
        c.width = w.width;
        c.height = w.height;
        c.fps = w.fps;
        if (Start(guid, c)) {
            captures_.emplace(guid, std::move(c));
            ApplyAutofocus(guid);
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
        if (guid != kFakeName) {
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
    std::unique_ptr<webrtc::VideoCaptureModule::DeviceInfo> info(webrtc::VideoCaptureFactory::CreateDeviceInfo());
    webrtc::VideoCaptureCapability requested, capability;
    requested.width = width;
    requested.height = height;
    requested.maxFPS = fps;
    requested.videoType = webrtc::VideoType::kI420;
    if (!info || info->GetBestMatchedCapability(guid.c_str(), requested, capability) < 0) capability = requested;
    info.reset();
    c.sink = std::make_unique<Sink>(source.get());
    c.module->RegisterCaptureDataCallback(c.sink.get());
    // a device that was just closed (another mode, another user) can still be busy for a moment
    bool started = false;
    for (int attempt = 0; attempt < 3 && !started; attempt++) {
        if (attempt) {
            VV_LOGW("camera", "cannot start %s, retrying", guid.c_str());
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        started = c.module->StartCapture(capability) == 0;
    }
    if (!started) {
        VV_LOGE("camera", "cannot start %s", guid.c_str());
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
