#include "video/video_sinks.h"

#include <api/video/i420_buffer.h>
#include <libyuv/convert_argb.h>

#include <memory>

#include "common/log.h"
#include <vector>

namespace vv {

VideoSinks& VideoSinks::Get()
{
    static auto* sinks = new VideoSinks();
    return *sinks;
}

void VideoSinks::SetAllocator(const Napi::Value& fn)
{
    if (fn.IsFunction()) allocator_ = Napi::Persistent(fn.As<Napi::Function>());
    else allocator_.Reset();
}

void VideoSinks::SetObserver(std::function<void(const std::string&, bool)> observer)
{
    std::lock_guard lock(mutex_);
    observer_ = std::move(observer);
}

void VideoSinks::SetSink(const std::string& streamId, JsCallbackPtr cb)
{
    bool attached = cb != nullptr, changed;
    std::function<void(const std::string&, bool)> observer;
    {
        std::lock_guard lock(mutex_);
        changed = attached != (sinks_.count(streamId) != 0);
        if (cb) sinks_[streamId] = Sink{std::move(cb), true};
        else sinks_.erase(streamId);
        observer = observer_;
    }
    if (changed && observer) observer(streamId, attached);
}

void VideoSinks::SignalReady(const std::string& streamId)
{
    std::lock_guard lock(mutex_);
    if (auto it = sinks_.find(streamId); it != sinks_.end()) it->second.ready = true;
}

bool VideoSinks::HasSink(const std::string& streamId) const
{
    std::lock_guard lock(mutex_);
    return sinks_.count(streamId) != 0;
}

void VideoSinks::Deliver(const std::string& streamId, const webrtc::VideoFrame& frame)
{
    JsCallbackPtr cb;
    {
        std::lock_guard lock(mutex_);
        auto it = sinks_.find(streamId);
        if (it == sinks_.end()) return;
        if (!it->second.ready) {
            if (++it->second.dropped % 300 == 1)
                VV_LOGI("video", "sink %s: waiting for signalVideoOutputSinkReady, %llu frames dropped", streamId.c_str(),
                        static_cast<unsigned long long>(it->second.dropped));
            return;
        }
        it->second.ready = false;
        cb = it->second.cb;
    }
    auto i420 = frame.video_frame_buffer()->ToI420();
    if (!i420) return;
    int w = i420->width(), h = i420->height();
    auto rgba = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(w) * h * 4);
    // libyuv "ABGR" is R, G, B, A in memory = ImageData's RGBA
    libyuv::I420ToABGR(i420->DataY(), i420->StrideY(), i420->DataU(), i420->StrideU(), i420->DataV(), i420->StrideV(),
                       rgba->data(), w * 4, w, h);
    cb->Call([this, rgba, w, h](Napi::Env env) {
        Napi::Value image;
        if (!allocator_.IsEmpty()) image = allocator_.Call({Napi::Number::New(env, w), Napi::Number::New(env, h)});
        Napi::Value data = image.IsObject() ? image.As<Napi::Object>().Get("data") : env.Undefined();
        if (data.IsTypedArray() && data.As<Napi::TypedArray>().ByteLength() == rgba->size()) {
            auto ta = data.As<Napi::TypedArray>();
            memcpy(static_cast<uint8_t*>(ta.ArrayBuffer().Data()) + ta.ByteOffset(), rgba->data(), rgba->size());
        } else {
            // no allocator (plain Node): an ImageData-like object
            auto arr = Napi::TypedArrayOf<uint8_t>::New(env, rgba->size(), napi_uint8_clamped_array);
            memcpy(arr.Data(), rgba->data(), rgba->size());
            auto obj = Napi::Object::New(env);
            obj.Set("width", w);
            obj.Set("height", h);
            obj.Set("data", arr);
            image = obj;
        }
        return JsCallback::Args{image};
    });
}

} // namespace vv
