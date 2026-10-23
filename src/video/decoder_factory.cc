#include "video/decoder_factory.h"

#include <api/video_codecs/sdp_video_format.h>
#include <modules/video_coding/codecs/av1/dav1d_decoder.h>
#include <modules/video_coding/codecs/vp8/include/vp8.h>
#include <modules/video_coding/codecs/vp9/include/vp9.h>

#include "video/ffmpeg.h"
#include "video/ffmpeg_encoder.h"

namespace vv {

std::string WebrtcCodecName(const std::string& name)
{
    if (name == "AV1X" || name == "AV1") return "AV1";
    if (name == "VP8" || name == "VP9" || name == "H264" || name == "H265") return name;
    return "";
}

std::vector<webrtc::SdpVideoFormat> DecoderFactory::GetSupportedFormats() const
{
    std::vector<webrtc::SdpVideoFormat> formats = {webrtc::SdpVideoFormat("VP8"), webrtc::SdpVideoFormat("VP9"),
                                                   webrtc::SdpVideoFormat("AV1")};
    if (FfmpegCanDecode(FfmpegCodec::H264)) formats.emplace_back("H264");
    if (FfmpegCanDecode(FfmpegCodec::H265)) formats.emplace_back("H265");
    return formats;
}

std::unique_ptr<webrtc::VideoDecoder> DecoderFactory::Create(const webrtc::Environment& env, const webrtc::SdpVideoFormat& format)
{
    const auto& n = format.name;
    if (n == "VP8") return webrtc::CreateVp8Decoder(env);
    if (n == "VP9") return webrtc::VP9Decoder::Create();
    if (n == "AV1") return webrtc::CreateDav1dDecoder(env);
    if (n == "H264") return CreateFfmpegDecoder(FfmpegCodec::H264);
    if (n == "H265") return CreateFfmpegDecoder(FfmpegCodec::H265);
    return nullptr;
}

json CodecCapabilities()
{
    // VP8 always (libvpx); H.264 / H.265 when an FFmpeg encoder really opens here (NVENC or the CPU)
    return json::array({
        {{"codec", "AV1"}, {"decode", true}, {"encode", false}},
        {{"codec", "H264"}, {"decode", FfmpegCanDecode(FfmpegCodec::H264)}, {"encode", FfmpegCanEncode(EncodeCodec::H264)}},
        {{"codec", "H265"}, {"decode", FfmpegCanDecode(FfmpegCodec::H265)}, {"encode", FfmpegCanEncode(EncodeCodec::H265)}},
        {{"codec", "VP8"}, {"decode", true}, {"encode", true}},
        {{"codec", "VP9"}, {"decode", true}, {"encode", false}},
    });
}

} // namespace vv
