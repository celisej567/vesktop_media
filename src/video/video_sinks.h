// Callback video sinks: decoded frames go to JS as RGBA ImageData, allocated by the
// function given to setImageDataAllocator. One frame in flight per stream: the next one is delivered
// only after signalVideoOutputSinkReady(streamId).
#pragma once

#include <api/video/video_frame.h>

#include <functional>
#include <map>
#include <mutex>
#include <string>

#include "common/js.h"

namespace vv {

class VideoSinks {
public:
    static VideoSinks& Get();

    void SetAllocator(const Napi::Value& fn); // JS thread
    void SetSink(const std::string& streamId, JsCallbackPtr cb);
    // Told when a stream id gets its first sink or loses its last one (camera preview)
    void SetObserver(std::function<void(const std::string& streamId, bool attached)> observer);
    void SignalReady(const std::string& streamId);
    bool HasSink(const std::string& streamId) const;
    // any thread
    void Deliver(const std::string& streamId, const webrtc::VideoFrame& frame);

private:
    struct Sink {
        JsCallbackPtr cb;
        bool ready = true;
        uint64_t dropped = 0;
    };
    mutable std::mutex mutex_;
    std::map<std::string, Sink> sinks_;
    std::function<void(const std::string&, bool)> observer_;
    Napi::FunctionReference allocator_; // JS thread only
};

} // namespace vv
