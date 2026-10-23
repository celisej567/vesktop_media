#pragma once

#include <chrono>
#include <cstdint>
#include <cstring>

namespace vv {

inline int64_t NowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

inline int64_t NowUs()
{
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

// NTP time (RFC 3550 sender reports): seconds since 1900 in 32.32 fixed point
inline uint64_t NtpNow()
{
    using namespace std::chrono;
    auto us = duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
    uint64_t secs = static_cast<uint64_t>(us / 1'000'000) + 2'208'988'800ull;
    uint64_t frac = (static_cast<uint64_t>(us % 1'000'000) << 32) / 1'000'000;
    return (secs << 32) | frac;
}

inline uint16_t ReadU16(const uint8_t* p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }
inline uint32_t ReadU32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 | p[3];
}
inline void WriteU16(uint8_t* p, uint16_t v)
{
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
}
inline void WriteU32(uint8_t* p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}
inline uint32_t ReadU32LE(const uint8_t* p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v; // all supported targets are little endian
}
inline void WriteU32LE(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }

} // namespace vv
