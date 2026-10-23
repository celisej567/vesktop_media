// Transport encryption of RTP/RTCP
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace vv {

enum class CryptoMode {
    None,
    AeadAes256GcmRtpsize,
    AeadAes256Gcm,
    AeadXChaCha20Poly1305Rtpsize,
    XSalsa20Poly1305LiteRtpsize,
    XSalsa20Poly1305Lite,
    XSalsa20Poly1305Suffix,
    XSalsa20Poly1305,
};

// In the order getEncryptionModes returns them (JS picks the first one the server also offers)
const std::vector<std::string>& SupportedEncryptionModes();
CryptoMode ParseCryptoMode(const std::string& name);
const char* CryptoModeName(CryptoMode mode);
bool IsRtpSizeMode(CryptoMode mode);

class TransportCrypto {
public:
    bool Configure(CryptoMode mode, const std::vector<uint8_t>& key);
    bool Active() const;
    CryptoMode Mode() const;

    // `unencrypted` = number of leading bytes kept in clear (and authenticated as AAD by AEAD modes).
    // `counter` is the per stream nonce counter, incremented here.
    bool Encrypt(const uint8_t* packet, size_t length, size_t unencrypted, uint32_t& counter, std::vector<uint8_t>& out) const;
    // out = clear header followed by the decrypted payload
    bool Decrypt(const uint8_t* packet, size_t length, size_t unencrypted, std::vector<uint8_t>& out) const;

    // Bytes kept in clear for an RTP packet in the current mode
    size_t RtpUnencryptedLength(const uint8_t* packet, size_t length) const;
    static constexpr size_t kRtcpUnencryptedLength = 8;

private:
    mutable std::mutex mutex_;
    CryptoMode mode_ = CryptoMode::None;
    std::vector<uint8_t> key_;
};

} // namespace vv
