// H.264 / H.265 encoding through FFmpeg (Electron's libffmpeg has no encoders): NVENC on NVIDIA
// (h264_nvenc / hevc_nvenc), AMF on AMD (Windows), VA-API on AMD / Intel (Linux), else the CPU (libx264 / libx265). Our libvesktop_ffmpeg.so (scripts/build-ffmpeg.sh)
// first, a system FFmpeg as the fallback, loaded at runtime; every encoder is opened once on a small test picture, and only the ones that open are offered (a GPU encoder can be compiled
// into FFmpeg without the GPU or driver to run it).
#pragma once

#include <api/video_codecs/video_encoder.h>

#include <memory>
#include <string>
#include <vector>

namespace vv {

enum class EncodeCodec { H264, H265 };

// "auto" (hardware first), "hardware", "software", or a family first: "nvenc", "amf", "vaapi" (the next in the
// usual order when it does not work)
void SetEncoderPreference(const std::string& preference);
std::string EncoderPreference();

// The user's encoder tuning (Vesktop extension setVideoEncoderTuning), for H.264 / H.265:
// preset "auto" (ours) | "speed" | "balanced" | "quality"; aq (adaptive quantization) "auto" (the encoder's default)
// | "on" | "off"; bufferMs: the rate control buffer, 0 = ours (half a second). An encoder that is open reopens
// with it (a key frame)
struct EncoderTuning {
    std::string preset = "auto";
    std::string aq = "auto";
    int bufferMs = 0;
};
void SetEncoderTuning(const EncoderTuning& tuning);
EncoderTuning GetEncoderTuning();

bool FfmpegCanEncode(EncodeCodec codec);
// The FFmpeg encoder that would be used ("h264_nvenc", "libx264"…), "" if none
std::string FfmpegEncoderName(EncodeCodec codec);
std::unique_ptr<webrtc::VideoEncoder> CreateFfmpegEncoder(EncodeCodec codec);

struct WorkingEncoder {
    std::string name;   // "hevc_vaapi"
    std::string family; // "nvenc", "amf", "vaapi", "software"
};
// The encoders that open here, in order of preference (probes each once)
std::vector<WorkingEncoder> FfmpegWorkingEncoders(EncodeCodec codec);

} // namespace vv
