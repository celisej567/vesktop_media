#include "video/frame_source.h"

#include <api/video/i420_buffer.h>
#include <api/video/video_adapter.h>
#include <rtc_base/time_utils.h>

#include <algorithm>
#include <chrono>

#include "common/log.h"
#include "video/video_sinks.h"

namespace vv {

FrameSource::FrameSource(bool screencast, std::string name) : screencast_(screencast), name_(std::move(name))
{
    if (screencast_) repeater_ = std::thread([this] { RepeatLoop(); });
}

FrameSource::~FrameSource()
{
    {
        std::lock_guard lock(repeatMutex_);
        stop_ = true;
    }
    repeatCv_.notify_all();
    if (repeater_.joinable()) repeater_.join();
}

void FrameSource::RepeatLoop()
{
    std::unique_lock lock(repeatMutex_);
    while (!stop_) {
        int64_t interval = 1'000'000 / std::max(1, fps_.load());
        int64_t due = lastUs_ + interval;
        int64_t now = webrtc::TimeMicros();
        if (!last_ || now < due) {
            repeatCv_.wait_for(lock, std::chrono::microseconds(last_ ? due - now : interval));
            continue;
        }
        // on the capture's clock (the last frame's timestamp plus the time since), whatever clock a capturer stamps with
        auto frame = *last_;
        lastTimestampUs_ += now - lastUs_;
        frame.set_timestamp_us(lastTimestampUs_);
        lastUs_ = now;
        lock.unlock();
        Deliver(frame); // not to the previews: they show the capture
        lock.lock();
    }
}

void FrameSource::SetPreviewStreamId(const std::string& id)
{
    std::lock_guard lock(previewMutex_);
    previewStreamId_ = id;
}

void FrameSource::SetStreamPreviewId(const std::string& id)
{
    std::lock_guard lock(previewMutex_);
    streamPreviewId_ = id;
}

void FrameSource::SetOutputFormat(int width, int height, int fps)
{
    // only the pixel count and the rate: an aspect ratio would make the adapter crop the frame
    std::optional<int> pixels;
    if (width > 0 && height > 0) pixels = width * height;
    video_adapter()->OnOutputFormatRequest(std::nullopt, pixels, fps > 0 ? std::optional<int>(fps) : std::nullopt);
    fps_ = fps > 0 ? fps : 30;
}

void FrameSource::PushFrame(const webrtc::VideoFrame& frame)
{
    if (++frames_ % 300 == 1) VV_LOGI("camera", "%s: %llu frames captured (%dx%d)", name_.c_str(), static_cast<unsigned long long>(frames_.load()), frame.width(), frame.height());
    lastWidth_ = frame.width();
    lastHeight_ = frame.height();

    std::string preview, streamPreview;
    {
        std::lock_guard lock(previewMutex_);
        preview = previewStreamId_;
        streamPreview = streamPreviewId_;
    }
    if (!preview.empty()) VideoSinks::Get().Deliver(preview, frame);
    if (!streamPreview.empty() && streamPreview != preview) VideoSinks::Get().Deliver(streamPreview, frame);

    if (screencast_) {
        std::lock_guard lock(repeatMutex_);
        last_ = frame;
        lastUs_ = webrtc::TimeMicros();
        lastTimestampUs_ = frame.timestamp_us() ? frame.timestamp_us() : lastUs_;
    }
    Deliver(frame);
}

void FrameSource::Deliver(const webrtc::VideoFrame& frame)
{
    std::lock_guard lock(deliverMutex_);
    int64_t timeUs = frame.timestamp_us() ? frame.timestamp_us() : webrtc::TimeMicros();
    // libwebrtc drops a frame whose timestamp (in ms) is not after the previous one's
    timeUs = std::max(timeUs, lastDeliveredUs_ + 1000);
    lastDeliveredUs_ = timeUs;
    int outWidth, outHeight, cropWidth, cropHeight, cropX, cropY;
    if (!AdaptFrame(frame.width(), frame.height(), timeUs, &outWidth, &outHeight, &cropWidth, &cropHeight, &cropX, &cropY))
        return; // dropped to meet the requested frame rate

    if (outWidth == frame.width() && outHeight == frame.height() && cropX == 0 && cropY == 0) {
        auto copy = frame;
        copy.set_timestamp_us(timeUs);
        OnFrame(copy);
        return;
    }
    auto scaled = webrtc::I420Buffer::Create(outWidth, outHeight);
    scaled->CropAndScaleFrom(*frame.video_frame_buffer()->ToI420(), cropX, cropY, cropWidth, cropHeight);
    OnFrame(webrtc::VideoFrame::Builder()
                .set_video_frame_buffer(scaled)
                .set_rotation(frame.rotation())
                .set_timestamp_us(timeUs)
                .build());
}

} // namespace vv
