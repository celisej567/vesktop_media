// Screen cast frames in GPU memory (DMA-BUF) to I420 in system memory, through EGL and OpenGL ES: the frame is
// imported as an external texture (any modifier the driver can import, compressed ones included), converted to
// I420 by a shader on the GPU and only the I420 planes are read back (1.5 bytes a pixel instead of 4, and no
// conversion on the CPU). libEGL is loaded at runtime; without it, or without DMA-BUF import, Init() fails and
// the capture stays on shared memory. One instance per capture: its context is used by one thread at a time.
#pragma once

#include <api/scoped_refptr.h>
#include <api/video/i420_buffer.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace vv {

class EglDmaBuf {
public:
    struct Plane {
        int fd;
        uint32_t offset;
        uint32_t stride;
    };

    EglDmaBuf();
    ~EglDmaBuf();
    EglDmaBuf(const EglDmaBuf&) = delete;
    EglDmaBuf& operator=(const EglDmaBuf&) = delete;

    // Creates the display and context; leaves no context current on the calling thread
    bool Init();
    // The modifiers the GPU can import for a DRM format; DRM_FORMAT_MOD_INVALID (implicit) only when it lists none
    std::vector<uint64_t> Modifiers(uint32_t drmFormat) const;
    // The frame as I420 (even width and height, at most the frame's). Makes the context current on the calling
    // thread. Null on failure
    webrtc::scoped_refptr<webrtc::I420Buffer> ReadI420(uint32_t drmFormat, uint64_t modifier, int width, int height,
                                                       const std::vector<Plane>& planes);
    // Forgets the imported image of a buffer, or the sync object of one of its sync fds (the stream removed it)
    void Forget(int fd);

    // Explicit sync (PipeWire's SyncTimeline): whether the GPU's DRM node has timeline sync objects
    bool ExplicitSync();
    // Waits (at most 100 ms) until the producer signalled `point` on the acquire timeline (its rendering is done)
    bool WaitAcquire(int syncFd, uint64_t point);
    // Signals `point` on the release timeline: the producer may reuse the buffer
    bool SignalRelease(int syncFd, uint64_t point);
    // Releases the context from the calling thread (before that thread ends), with its GL objects
    void Release();

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace vv
