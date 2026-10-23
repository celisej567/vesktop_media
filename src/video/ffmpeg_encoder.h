// H.264 / H.265 encoding through FFmpeg (Electron's libffmpeg has no encoders): NVENC on NVIDIA
// (h264_nvenc / hevc_nvenc), else the CPU (libx264 / libx265). Our libvesktop_ffmpeg.so (scripts/build-ffmpeg.sh)
// first, a system FFmpeg as the fallback, loaded at runtime; every encoder is opened once on a small test picture, and only the ones that open are offered (a GPU encoder can be compiled
// into FFmpeg without the GPU or driver to run it).
#pragma once

#include <api/video_codecs/video_encoder.h>

#include <memory>
#include <string>

namespace vv {

enum class EncodeCodec { H264, H265 };

// "auto" (hardware first), "hardware" or "software"
void SetEncoderPreference(const std::string& preference);
std::string EncoderPreference();

bool FfmpegCanEncode(EncodeCodec codec);
// The FFmpeg encoder that would be used ("h264_nvenc", "libx264"…), "" if none
std::string FfmpegEncoderName(EncodeCodec codec);
std::unique_ptr<webrtc::VideoEncoder> CreateFfmpegEncoder(EncodeCodec codec);

} // namespace vv
