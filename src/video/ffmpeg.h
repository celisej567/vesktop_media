// H.264 / H.265 decoding through FFmpeg: our own build next to the module first, then the host process' libffmpeg
// (Electron ships Chromium's, with H.264), then a system libavcodec. Loaded at runtime, used through the stable
// public API plus the long unchanged leading fields of AVPacket / AVFrame.
#pragma once

#include <api/video_codecs/video_decoder.h>

#include <memory>
#include <string>

namespace vv {

enum class FfmpegCodec { H264, H265 };

bool FfmpegCanDecode(FfmpegCodec codec);
std::unique_ptr<webrtc::VideoDecoder> CreateFfmpegDecoder(FfmpegCodec codec);

} // namespace vv
