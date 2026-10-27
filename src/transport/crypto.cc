#include "transport/crypto.h"

#include <openssl/aead.h>
#include <openssl/rand.h>

#include <algorithm>
#include <cstring>

#include "common/log.h"
#include "common/platform.h"
#include "common/util.h"

namespace vv {

namespace {

constexpr size_t kTagLength = 16;
constexpr size_t kSecretboxMac = 16;
constexpr size_t kSecretboxNonce = 24;

struct ModeName {
    CryptoMode mode;
    const char* name;
};

constexpr ModeName kModes[] = {
    {CryptoMode::AeadAes256GcmRtpsize, "aead_aes256_gcm_rtpsize"},
    {CryptoMode::AeadAes256Gcm, "aead_aes256_gcm"},
    {CryptoMode::AeadXChaCha20Poly1305Rtpsize, "aead_xchacha20_poly1305_rtpsize"},
    {CryptoMode::XSalsa20Poly1305LiteRtpsize, "xsalsa20_poly1305_lite_rtpsize"},
    {CryptoMode::XSalsa20Poly1305Lite, "xsalsa20_poly1305_lite"},
    {CryptoMode::XSalsa20Poly1305Suffix, "xsalsa20_poly1305_suffix"},
    {CryptoMode::XSalsa20Poly1305, "xsalsa20_poly1305"},
};

// AEAD modes use BoringSSL (part of libwebrtc). The legacy xsalsa20 secretbox modes (no longer offered
// by Discord's servers) need libsodium, loaded only when such a mode is selected.
bool Aead(bool seal, const EVP_AEAD* aead, const uint8_t* key, const uint8_t* nonce, size_t nonceLen, const uint8_t* ad,
          size_t adLen, const uint8_t* in, size_t inLen, std::vector<uint8_t>& out, size_t outOffset)
{
    EVP_AEAD_CTX ctx;
    if (!EVP_AEAD_CTX_init(&ctx, aead, key, 32, EVP_AEAD_DEFAULT_TAG_LENGTH, nullptr)) return false;
    size_t maxOut = seal ? inLen + EVP_AEAD_max_overhead(aead) : inLen;
    out.resize(outOffset + maxOut);
    size_t written = 0;
    int ok = seal ? EVP_AEAD_CTX_seal(&ctx, out.data() + outOffset, &written, maxOut, nonce, nonceLen, in, inLen, ad, adLen)
                  : EVP_AEAD_CTX_open(&ctx, out.data() + outOffset, &written, maxOut, nonce, nonceLen, in, inLen, ad, adLen);
    EVP_AEAD_CTX_cleanup(&ctx);
    if (!ok) return false;
    out.resize(outOffset + written);
    return true;
}

struct Sodium {
    int (*init)() = nullptr;
    int (*easy)(unsigned char*, const unsigned char*, unsigned long long, const unsigned char*, const unsigned char*) = nullptr;
    int (*open)(unsigned char*, const unsigned char*, unsigned long long, const unsigned char*, const unsigned char*) = nullptr;

    static const Sodium* Get()
    {
        static Sodium s = [] {
            Sodium r;
            void* h = OpenLibrary({"libsodium.so.26", "libsodium.so.23", "libsodium.so", "libsodium.dll"});
            if (!h) {
                VV_LOGE("crypto", "xsalsa20_poly1305 modes need libsodium, which was not found");
                return r;
            }
            r.init = reinterpret_cast<int (*)()>(LibrarySymbol(h, "sodium_init"));
            r.easy = reinterpret_cast<decltype(r.easy)>(LibrarySymbol(h, "crypto_secretbox_easy"));
            r.open = reinterpret_cast<decltype(r.open)>(LibrarySymbol(h, "crypto_secretbox_open_easy"));
            if (r.init) r.init();
            return r;
        }();
        return s.easy && s.open ? &s : nullptr;
    }
};

} // namespace

const std::vector<std::string>& SupportedEncryptionModes()
{
    static const std::vector<std::string> modes = [] {
        std::vector<std::string> v;
        for (auto& m : kModes) v.emplace_back(m.name);
        return v;
    }();
    return modes;
}

CryptoMode ParseCryptoMode(const std::string& name)
{
    for (auto& m : kModes)
        if (name == m.name) return m.mode;
    return CryptoMode::None;
}

const char* CryptoModeName(CryptoMode mode)
{
    for (auto& m : kModes)
        if (m.mode == mode) return m.name;
    return "none";
}

bool IsRtpSizeMode(CryptoMode mode)
{
    return mode == CryptoMode::AeadAes256GcmRtpsize || mode == CryptoMode::AeadXChaCha20Poly1305Rtpsize ||
           mode == CryptoMode::XSalsa20Poly1305LiteRtpsize;
}

bool TransportCrypto::Configure(CryptoMode mode, const std::vector<uint8_t>& key)
{
    if (mode == CryptoMode::None || key.size() != 32) return false;
    std::lock_guard lock(mutex_);
    mode_ = mode;
    key_ = key;
    return true;
}

bool TransportCrypto::Active() const
{
    std::lock_guard lock(mutex_);
    return mode_ != CryptoMode::None;
}

CryptoMode TransportCrypto::Mode() const
{
    std::lock_guard lock(mutex_);
    return mode_;
}

size_t TransportCrypto::RtpUnencryptedLength(const uint8_t* p, size_t length) const
{
    if (length < 12) return length;
    size_t len = 12 + 4 * (p[0] & 0x0f);
    // rtpsize modes keep the extension preamble ("BEDE LLLL") in clear, its values are encrypted
    if (IsRtpSizeMode(Mode()) && (p[0] & 0x10)) len += 4;
    return std::min(len, length);
}

bool TransportCrypto::Encrypt(const uint8_t* packet, size_t length, size_t unencrypted, uint32_t& counter,
                              std::vector<uint8_t>& out) const
{
    std::lock_guard lock(mutex_);
    if (mode_ == CryptoMode::None || unencrypted > length) return false;
    const uint8_t* plain = packet + unencrypted;
    size_t plainLen = length - unencrypted;

    out.assign(packet, packet + unencrypted);
    uint8_t nonce4[4];
    WriteU32LE(nonce4, counter);

    switch (mode_) {
    case CryptoMode::AeadAes256GcmRtpsize:
    case CryptoMode::AeadAes256Gcm: {
        counter++;
        uint8_t iv[12] = {};
        memcpy(iv, nonce4, 4);
        if (!Aead(true, EVP_aead_aes_256_gcm(), key_.data(), iv, sizeof(iv), packet, unencrypted, plain, plainLen, out, unencrypted))
            return false;
        out.insert(out.end(), nonce4, nonce4 + 4);
        return true;
    }
    case CryptoMode::AeadXChaCha20Poly1305Rtpsize: {
        counter++;
        uint8_t nonce[24] = {};
        memcpy(nonce, nonce4, 4);
        if (!Aead(true, EVP_aead_xchacha20_poly1305(), key_.data(), nonce, sizeof(nonce), packet, unencrypted, plain, plainLen,
                  out, unencrypted))
            return false;
        out.insert(out.end(), nonce4, nonce4 + 4);
        return true;
    }
    case CryptoMode::XSalsa20Poly1305LiteRtpsize:
    case CryptoMode::XSalsa20Poly1305Lite:
    case CryptoMode::XSalsa20Poly1305Suffix:
    case CryptoMode::XSalsa20Poly1305: {
        auto* na = Sodium::Get();
        if (!na) return false;
        uint8_t nonce[kSecretboxNonce] = {};
        if (mode_ == CryptoMode::XSalsa20Poly1305Suffix) {
            RAND_bytes(nonce, sizeof(nonce));
        } else if (mode_ == CryptoMode::XSalsa20Poly1305) {
            memcpy(nonce, packet, std::min<size_t>(12, unencrypted));
        } else {
            counter++;
            memcpy(nonce, nonce4, 4);
        }
        out.resize(unencrypted + kSecretboxMac + plainLen);
        na->easy(out.data() + unencrypted, plain, plainLen, nonce, key_.data());
        if (mode_ == CryptoMode::XSalsa20Poly1305Suffix) out.insert(out.end(), nonce, nonce + sizeof(nonce));
        else if (mode_ != CryptoMode::XSalsa20Poly1305) out.insert(out.end(), nonce4, nonce4 + 4);
        return true;
    }
    case CryptoMode::None: break;
    }
    return false;
}

bool TransportCrypto::Decrypt(const uint8_t* packet, size_t length, size_t unencrypted, std::vector<uint8_t>& out) const
{
    std::lock_guard lock(mutex_);
    if (mode_ == CryptoMode::None || unencrypted > length) return false;
    out.assign(packet, packet + unencrypted);

    switch (mode_) {
    case CryptoMode::AeadAes256GcmRtpsize:
    case CryptoMode::AeadAes256Gcm: {
        if (length < unencrypted + kTagLength + 4) return false;
        uint8_t iv[12] = {};
        memcpy(iv, packet + length - 4, 4);
        return Aead(false, EVP_aead_aes_256_gcm(), key_.data(), iv, sizeof(iv), packet, unencrypted, packet + unencrypted,
                    length - unencrypted - 4, out, unencrypted);
    }
    case CryptoMode::AeadXChaCha20Poly1305Rtpsize: {
        if (length < unencrypted + kTagLength + 4) return false;
        uint8_t nonce[24] = {};
        memcpy(nonce, packet + length - 4, 4);
        return Aead(false, EVP_aead_xchacha20_poly1305(), key_.data(), nonce, sizeof(nonce), packet, unencrypted,
                    packet + unencrypted, length - unencrypted - 4, out, unencrypted);
    }
    case CryptoMode::XSalsa20Poly1305LiteRtpsize:
    case CryptoMode::XSalsa20Poly1305Lite:
    case CryptoMode::XSalsa20Poly1305Suffix:
    case CryptoMode::XSalsa20Poly1305: {
        auto* na = Sodium::Get();
        if (!na) return false;
        uint8_t nonce[kSecretboxNonce] = {};
        size_t suffix = 0;
        if (mode_ == CryptoMode::XSalsa20Poly1305Suffix) {
            suffix = kSecretboxNonce;
            if (length < unencrypted + suffix) return false;
            memcpy(nonce, packet + length - suffix, suffix);
        } else if (mode_ == CryptoMode::XSalsa20Poly1305) {
            memcpy(nonce, packet, std::min<size_t>(12, unencrypted));
        } else {
            suffix = 4;
            if (length < unencrypted + suffix) return false;
            memcpy(nonce, packet + length - 4, 4);
        }
        if (length < unencrypted + suffix + kSecretboxMac) return false;
        size_t cLen = length - unencrypted - suffix;
        out.resize(unencrypted + cLen - kSecretboxMac);
        return na->open(out.data() + unencrypted, packet + unencrypted, cLen, nonce, key_.data()) == 0;
    }
    case CryptoMode::None: break;
    }
    return false;
}

} // namespace vv
