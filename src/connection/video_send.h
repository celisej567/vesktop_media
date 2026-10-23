// Pieces of the video send path (connection/video_send.cc)
#pragma once

#include <api/crypto/frame_encryptor_interface.h>
#include <api/video_codecs/video_encoder_factory.h>

#include <memory>
#include <string>

namespace vv {

class Connection;

// DAVE encryption of our encoded video frames (passthrough while DAVE is off)
class VideoFrameEncryptor : public webrtc::FrameEncryptorInterface {
public:
    explicit VideoFrameEncryptor(Connection* c) : c_(c) {}
    int Encrypt(webrtc::MediaType type, uint32_t ssrc, std::span<const uint8_t> additional, std::span<const uint8_t> frame,
                std::span<uint8_t> out, size_t* written) override;
    size_t GetMaxCiphertextByteSize(webrtc::MediaType type, size_t frameSize) override;

private:
    Connection* c_;
};

// Encoders we can create (VP8: libvpx, H.264 / H.265: FFmpeg), each wrapped in a SimulcastEncoderAdapter for the simulcast layers
std::unique_ptr<webrtc::VideoEncoderFactory> CreateVideoEncoderFactory();
// Discord codec name ("AV1X", "H264", …) we can encode, as libwebrtc names it; "" if none
std::string EncodableCodec(const std::string& discordName);

} // namespace vv
