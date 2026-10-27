#include "capture/desktop_capture_win.h"

#include <windows.h>

#include <objbase.h>
#include <wincodec.h>

#include <api/make_ref_counted.h>
#include <api/video/i420_buffer.h>
#include <libyuv/convert.h>
#include <libyuv/scale_argb.h>
#include <modules/desktop_capture/desktop_capture_options.h>
#include <modules/desktop_capture/desktop_capturer.h>
#include <modules/desktop_capture/desktop_frame.h>
#include <openssl/base64.h>
#include <rtc_base/time_utils.h>

#include <algorithm>
#include <chrono>
#include <vector>

#include "common/log.h"

namespace vv {

namespace {

// COM on the calling thread for as long as the object lives (WIC, and the capturers' own COM use)
struct ComScope {
    ComScope() { ok = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)); }
    ~ComScope()
    {
        if (ok) CoUninitialize();
    }
    bool ok = false;
};

template <typename T>
struct ComPtr {
    T* p = nullptr;
    ~ComPtr()
    {
        if (p) p->Release();
    }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

std::unique_ptr<webrtc::DesktopCapturer> CreateCapturer(DesktopCaptureWin::Kind kind, bool forPreview)
{
    auto options = webrtc::DesktopCaptureOptions::CreateDefault();
    options.set_allow_directx_capturer(true);
    if (kind == DesktopCaptureWin::Kind::Screen) return webrtc::DesktopCapturer::CreateScreenCapturer(options);
    // Windows.Graphics.Capture sees what GDI cannot (GPU-drawn windows: browsers, games, Electron apps). It shows
    // a yellow border on Windows 10 and needs a moment per window to start, so the thumbnails take GDI's copy
    options.set_allow_wgc_window_capturer(!forPreview);
    options.set_allow_wgc_capturer_fallback(true);
    options.set_allow_cropping_window_capturer(true);
    return webrtc::DesktopCapturer::CreateWindowCapturer(options);
}

// Delivers CaptureFrame()'s result to whoever waits on it (the capturers call back from CaptureFrame itself)
class FrameCallback : public webrtc::DesktopCapturer::Callback {
public:
    void OnCaptureResult(webrtc::DesktopCapturer::Result result, std::unique_ptr<webrtc::DesktopFrame> frame) override
    {
        result_ = result;
        frame_ = std::move(frame);
    }
    webrtc::DesktopCapturer::Result result_ = webrtc::DesktopCapturer::Result::ERROR_TEMPORARY;
    std::unique_ptr<webrtc::DesktopFrame> frame_;
};

// A frame as a PNG of at most width x height (aspect kept), as a data URL; "" when it cannot be encoded
std::string Thumbnail(const webrtc::DesktopFrame& frame, int maxWidth, int maxHeight)
{
    int srcW = frame.size().width(), srcH = frame.size().height();
    if (srcW <= 0 || srcH <= 0 || maxWidth <= 0 || maxHeight <= 0) return "";
    double scale = std::min({1.0, static_cast<double>(maxWidth) / srcW, static_cast<double>(maxHeight) / srcH});
    int w = std::max(1, static_cast<int>(srcW * scale)), h = std::max(1, static_cast<int>(srcH * scale));
    std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4);
    libyuv::ARGBScale(frame.data(), frame.stride(), srcW, srcH, pixels.data(), w * 4, w, h, libyuv::kFilterBox);
    // the captures leave alpha undefined (often 0): the thumbnail would be transparent
    for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 255;

    ComPtr<IWICImagingFactory> factory;
    ComPtr<IStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> target;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) ||
        FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.p, WICBitmapEncoderNoCache)) || FAILED(encoder->CreateNewFrame(&target, nullptr)) ||
        FAILED(target->Initialize(nullptr)) || FAILED(target->SetSize(w, h)))
        return "";
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(target->SetPixelFormat(&format)) || format != GUID_WICPixelFormat32bppBGRA ||
        FAILED(target->WritePixels(h, w * 4, static_cast<UINT>(pixels.size()), pixels.data())) || FAILED(target->Commit()) ||
        FAILED(encoder->Commit()))
        return "";

    HGLOBAL memory = nullptr;
    if (FAILED(GetHGlobalFromStream(stream.p, &memory))) return "";
    STATSTG stat{};
    stream->Stat(&stat, STATFLAG_NONAME);
    size_t size = static_cast<size_t>(stat.cbSize.QuadPart);
    const auto* png = static_cast<const uint8_t*>(GlobalLock(memory));
    if (!png) return "";
    size_t encodedSize = 0;
    EVP_EncodedLength(&encodedSize, size);
    std::string url = "data:image/png;base64,";
    size_t prefix = url.size();
    url.resize(prefix + encodedSize);
    size_t written = EVP_EncodeBlock(reinterpret_cast<uint8_t*>(url.data() + prefix), png, size);
    GlobalUnlock(memory);
    url.resize(prefix + written);
    return url;
}

// One frame of the selected source (DXGI's first frame can be a temporary error)
std::unique_ptr<webrtc::DesktopFrame> CaptureOne(webrtc::DesktopCapturer& capturer, FrameCallback& callback)
{
    for (int attempt = 0; attempt < 3; attempt++) {
        callback.frame_.reset();
        capturer.CaptureFrame();
        if (callback.result_ == webrtc::DesktopCapturer::Result::SUCCESS && callback.frame_) return std::move(callback.frame_);
        if (callback.result_ == webrtc::DesktopCapturer::Result::ERROR_PERMANENT) break;
        Sleep(30);
    }
    return nullptr;
}

} // namespace

json DesktopPreviews(bool windows, int width, int height)
{
    ComScope com;
    json list = json::array();
    auto kind = windows ? DesktopCaptureWin::Kind::Window : DesktopCaptureWin::Kind::Screen;
    auto capturer = CreateCapturer(kind, true);
    if (!capturer) return list;
    FrameCallback callback;
    capturer->Start(&callback);
    webrtc::DesktopCapturer::SourceList sources;
    if (!capturer->GetSourceList(&sources)) return list;
    for (auto& source : sources) {
        std::string url;
        if (capturer->SelectSource(source.id))
            if (auto frame = CaptureOne(*capturer, callback)) url = Thumbnail(*frame, width, height);
        // a window that shows nothing (minimized, cloaked) is still offered: Electron's picker does too
        std::string id = std::string(windows ? "window:" : "screen:") + std::to_string(source.id) + ":0";
        list.push_back({{"id", id}, {"name", source.title}, {"url", url}});
    }
    VV_LOGI("capture", "%zu %s previews", list.size(), windows ? "window" : "screen");
    return list;
}

DesktopCaptureWin::DesktopCaptureWin(Kind kind, int64_t id, webrtc::scoped_refptr<FrameSource> sink,
                                     std::function<void(const std::string&)> ended)
    : kind_(kind), id_(id), sink_(std::move(sink)), ended_(std::move(ended))
{
}

DesktopCaptureWin::~DesktopCaptureWin()
{
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

bool DesktopCaptureWin::Start()
{
    // the capturer lives on the capture thread (it must be used and destroyed on one thread): wait for it to open
    auto started = std::make_shared<std::atomic<int>>(0); // 0 pending, 1 ok, -1 failed
    running_ = true;
    thread_ = std::thread([this, started] { Loop(started); });
    while (*started == 0) Sleep(5);
    if (*started < 0) {
        running_ = false;
        thread_.join();
        return false;
    }
    return true;
}

void DesktopCaptureWin::SetEnded(std::function<void(const std::string&)> ended)
{
    std::string reason;
    {
        std::lock_guard lock(endedMutex_);
        ended_ = std::move(ended);
        if (!hasEnded_) return;
        reason = endReason_;
    }
    if (ended_) ended_(reason);
}

void DesktopCaptureWin::End(const std::string& reason)
{
    std::function<void(const std::string&)> ended;
    {
        std::lock_guard lock(endedMutex_);
        if (hasEnded_) return;
        hasEnded_ = true;
        endReason_ = reason;
        ended = ended_;
    }
    VV_LOGI("capture", "%s %lld ended: %s", kind_ == Kind::Screen ? "screen" : "window", static_cast<long long>(id_), reason.c_str());
    if (ended) ended(reason);
}

void DesktopCaptureWin::Loop(std::shared_ptr<std::atomic<int>> started)
{
    ComScope com;
    SetThreadDescription(GetCurrentThread(), L"vv desktop capture");
    auto capturer = CreateCapturer(kind_, false);
    FrameCallback callback;
    if (capturer) capturer->Start(&callback);
    if (!capturer || !capturer->SelectSource(static_cast<webrtc::DesktopCapturer::SourceId>(id_))) {
        VV_LOGE("capture", "cannot capture %s %lld", kind_ == Kind::Screen ? "screen" : "window", static_cast<long long>(id_));
        *started = -1;
        return;
    }
    VV_LOGI("capture", "capturing %s %lld", kind_ == Kind::Screen ? "screen" : "window", static_cast<long long>(id_));
    *started = 1;

    // 60 frames a second at most; FrameSource drops what the encoder does not want
    constexpr auto kInterval = std::chrono::microseconds(1'000'000 / 60);
    auto next = std::chrono::steady_clock::now();
    int failures = 0;
    const HWND window = kind_ == Kind::Window ? reinterpret_cast<HWND>(static_cast<intptr_t>(id_)) : nullptr;
    while (running_) {
        // a closed window can still hand out its last frame for a while (Windows.Graphics.Capture)
        if (window && !IsWindow(window)) {
            End("the window was closed");
            break;
        }
        callback.frame_.reset();
        capturer->CaptureFrame();
        if (callback.result_ == webrtc::DesktopCapturer::Result::SUCCESS && callback.frame_) {
            failures = 0;
            auto& f = *callback.frame_;
            int w = f.size().width(), h = f.size().height();
            if (w > 1 && h > 1) {
                // DesktopFrame is BGRA in memory, libyuv's "ARGB"
                auto buffer = webrtc::I420Buffer::Create(w, h);
                libyuv::ARGBToI420(f.data(), f.stride(), buffer->MutableDataY(), buffer->StrideY(), buffer->MutableDataU(),
                                   buffer->StrideU(), buffer->MutableDataV(), buffer->StrideV(), w, h);
                sink_->PushFrame(webrtc::VideoFrame::Builder().set_video_frame_buffer(buffer).set_timestamp_us(webrtc::TimeMicros()).build());
                frames_++;
            }
        } else if (callback.result_ == webrtc::DesktopCapturer::Result::ERROR_PERMANENT) {
            End(kind_ == Kind::Window ? "the window was closed" : "the screen is gone");
            break;
        } else if (++failures == 300) {
            // temporary errors for 5 s (a screen that went away, a window that stays minimized is not one: it
            // repeats its last frame)
            VV_LOGW("capture", "no frames for 5 s");
        }
        next += kInterval;
        auto now = std::chrono::steady_clock::now();
        if (next < now) next = now; // behind (a slow capture): do not try to catch up
        std::this_thread::sleep_until(next);
    }
}

} // namespace vv
