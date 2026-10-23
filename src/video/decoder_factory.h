// Video decoders: VP8, VP9, AV1 (dav1d) from libwebrtc, H.264 / H.265 from FFmpeg
#pragma once

#include <api/video_codecs/video_decoder_factory.h>

#include <string>
#include <vector>

#include "common/js.h"

namespace vv {

class DecoderFactory : public webrtc::VideoDecoderFactory {
public:
    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override;
    std::unique_ptr<webrtc::VideoDecoder> Create(const webrtc::Environment& env, const webrtc::SdpVideoFormat& format) override;
};

// Discord's codec name ("AV1X" is AV1) -> WebRTC name, "" if unsupported
std::string WebrtcCodecName(const std::string& discordName);
// getCodecCapabilities JSON array
json CodecCapabilities();

} // namespace vv
