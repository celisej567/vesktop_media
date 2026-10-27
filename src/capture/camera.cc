#include "capture/camera.h"

#include <api/make_ref_counted.h>
#include <api/video/i420_buffer.h>
#include <modules/video_capture/video_capture_factory.h>
#include <rtc_base/time_utils.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

#include "capture/camera_devices.h"
#include "common/log.h"
#include "common/platform.h"

namespace vv {

namespace {

constexpr const char* kFakeName = "Strobe Video Source";
constexpr const char* kFakeName2 = "Strobe Video Source 2"; // a second one, to switch between (the tests)

using camera_devices::DeviceEntry;

std::vector<DeviceEntry> ListDevices(bool fake)
{
    if (!fake) return camera_devices::List();
    std::vector<DeviceEntry> out;
    for (const char* name : {kFakeName, kFakeName2})
        out.push_back({name, name, "", {{1920, 1080, 30}, {1280, 720, 60}, {1280, 720, 30}, {640, 360, 30}}});
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

// A Y4M file (8-bit 4:2:0) read frame by frame, looped: the fake camera with useFileForFakeVideoCapture
class Y4mReader {
public:
    explicit Y4mReader(const std::string& path) : file_(OpenFile(path, "rb"))
    {
        if (file_ && !ParseHeader()) Close();
    }
    ~Y4mReader() { Close(); }

    bool Ok() const { return file_ != nullptr; }
    int Width() const { return width_; }
    int Height() const { return height_; }
    double Fps() const { return fps_; }

    webrtc::scoped_refptr<webrtc::I420Buffer> Next()
    {
        char line[128];
        if (!fgets(line, sizeof(line), file_)) {
            fseek(file_, data_, SEEK_SET); // loop
            if (!fgets(line, sizeof(line), file_)) return nullptr;
        }
        if (strncmp(line, "FRAME", 5) != 0) return nullptr;
        auto buf = webrtc::I420Buffer::Create(width_, height_);
        auto plane = [&](uint8_t* dst, int stride, int w, int h) {
            for (int y = 0; y < h; y++)
                if (fread(dst + y * stride, 1, w, file_) != static_cast<size_t>(w)) return false;
            return true;
        };
        int cw = (width_ + 1) / 2, ch = (height_ + 1) / 2;
        if (!plane(buf->MutableDataY(), buf->StrideY(), width_, height_) || !plane(buf->MutableDataU(), buf->StrideU(), cw, ch) ||
            !plane(buf->MutableDataV(), buf->StrideV(), cw, ch))
            return nullptr;
        return buf;
    }

private:
    bool ParseHeader()
    {
        char header[256];
        if (!fgets(header, sizeof(header), file_) || strncmp(header, "YUV4MPEG2 ", 10) != 0) return false;
        for (char* t = strtok(header + 10, " \n"); t; t = strtok(nullptr, " \n")) {
            if (t[0] == 'W') width_ = atoi(t + 1);
            else if (t[0] == 'H') height_ = atoi(t + 1);
            else if (t[0] == 'F') {
                int num = 0, den = 1;
                if (sscanf(t + 1, "%d:%d", &num, &den) == 2 && num > 0 && den > 0) fps_ = static_cast<double>(num) / den;
            } else if (t[0] == 'C' && strncmp(t + 1, "420", 3) != 0) return false;
        }
        data_ = ftell(file_);
        return width_ > 0 && height_ > 0;
    }

    void Close()
    {
        if (file_) fclose(file_);
        file_ = nullptr;
    }

    FILE* file_;
    int width_ = 0, height_ = 0;
    double fps_ = 30;
    long data_ = 0;
};

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
            d.hasAutofocus = camera_devices::HasAutofocus({d.name, d.guid, d.path, {}});
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
    DeviceEntry device;
    {
        std::lock_guard lock(mutex_);
        auto it = options_.find(guid);
        if (it == options_.end() || it->second.autofocus < 0) return;
        value = it->second.autofocus;
        for (auto& d : devices_)
            if (d.guid == guid) device = {d.name, d.guid, d.path, {}};
    }
    if (device.guid.empty()) return;
    if (!camera_devices::SetAutofocus(device, value != 0)) VV_LOGW("camera", "%s: cannot set autofocus %d", guid.c_str(), value);
    else VV_LOGI("camera", "%s: autofocus %s", guid.c_str(), value ? "on" : "off");
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
    // Also while a camera captures: listing sends nothing to the cameras (EnumerateModes), so a running capture is not
    // disturbed, and the list follows cameras that come and go (OBS Virtual Camera switched off) in a call too
    Post([this] {
        if (fake_) {
            if (!ready_) Enumerate();
            return;
        }
        auto sig = camera_devices::Signature();
        if (ready_ && sig == signature_) {
            pendingSignature_.clear();
            return;
        }
        auto now = std::chrono::steady_clock::now();
        if (ready_) {
            // A camera that is plugging in or resetting makes its nodes come and go every few seconds: wait until
            // they stay as they are for one more poll, and not more often than every 3 s
            if (sig != pendingSignature_) {
                if (pendingSignature_.empty()) VV_LOGI("camera", "the camera nodes changed, waiting for them to settle");
                pendingSignature_ = sig;
                return;
            }
            if (now - lastEnumerated_ < std::chrono::seconds(3)) return;
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
            signature_ = fake_ ? "" : camera_devices::Signature();
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
        std::shared_ptr<Y4mReader> file;
        if (!fakeFile_.empty()) {
            file = std::make_shared<Y4mReader>(fakeFile_);
            if (!file->Ok()) {
                VV_LOGE("camera", "cannot read fake video file %s (Y4M, 8-bit 4:2:0)", fakeFile_.c_str());
                file.reset();
            } else {
                width = file->Width(), height = file->Height(), fps = std::max(1, static_cast<int>(std::lround(file->Fps())));
            }
        }
        c.fakeRunning = std::make_shared<std::atomic<bool>>(true);
        c.fakeThread = std::thread([source, running = c.fakeRunning, width, height, fps, file] {
            int n = 0;
            auto next = std::chrono::steady_clock::now();
            while (*running) {
                webrtc::scoped_refptr<webrtc::I420Buffer> buf = file ? file->Next() : FakeFrame(width, height, n++);
                if (!buf) break;
                source->PushFrame(webrtc::VideoFrame::Builder()
                                      .set_video_frame_buffer(buf)
                                      .set_timestamp_us(webrtc::TimeMicros())
                                      .build());
                next += std::chrono::microseconds(1'000'000 / fps);
                std::this_thread::sleep_until(next);
            }
        });
        VV_LOGI("camera", "fake camera started: %dx%d @ %d%s%s", width, height, fps, file ? " from " : "", file ? fakeFile_.c_str() : "");
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
