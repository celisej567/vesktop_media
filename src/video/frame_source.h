// A video source for VideoSendStream (camera or desktop capture). Capturers push frames from any thread;
// the source scales them to what the encoder asks for (sink wants) and also hands them to the JS preview
// sink of its stream id, when one is attached.
#pragma once

#include <api/video/adapted_video_track_source.h>
#include <api/video/video_frame.h>

#include <atomic>
#include <mutex>
#include <string>

namespace vv {

class FrameSource : public webrtc::AdaptedVideoTrackSource {
public:
    FrameSource(bool screencast, std::string name) : screencast_(screencast), name_(std::move(name)) {}

    void PushFrame(const webrtc::VideoFrame& frame);
    // Stream id whose JS sink gets the (unscaled) frames, "" for none
    void SetPreviewStreamId(const std::string& id);
    // A second one: a camera streamed with Go Live also previews as the stream ("" for none)
    void SetStreamPreviewId(const std::string& id);
    // Caps the frame size and rate (encodingVideoWidth / Height / FrameRate), 0 = unlimited
    void SetOutputFormat(int width, int height, int fps);
    uint64_t FramesPushed() const { return frames_; }
    int LastWidth() const { return lastWidth_; }
    int LastHeight() const { return lastHeight_; }
    const std::string& Name() const { return name_; }

    // VideoTrackSourceInterface
    SourceState state() const override { return kLive; }
    bool remote() const override { return false; }
    bool is_screencast() const override { return screencast_; }
    std::optional<bool> needs_denoising() const override { return std::nullopt; }

private:
    const bool screencast_;
    const std::string name_;
    std::mutex previewMutex_;
    std::string previewStreamId_, streamPreviewId_;
    std::atomic<uint64_t> frames_{0};
    std::atomic<int> lastWidth_{0}, lastHeight_{0};
};

} // namespace vv
