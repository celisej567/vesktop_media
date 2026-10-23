// 32-bit RGB -> I420 (BT.601, limited range), as the encoder and every viewer expect.
// libyuv's ARGBToI420 truncates the chroma: U and V come out 0.5 low on average. One pass is invisible, but a
// stream that shows itself (or a viewer's screen being streamed back) accumulates it and turns green within a
// few generations. Y is taken from libyuv (accurate); U and V are recomputed here with rounding.
#pragma once

#include <libyuv.h>

#include <cstdint>

namespace vv {

// `bgr`: B, G, R, x in memory (libyuv ARGB, PipeWire BGRx/BGRA); otherwise R, G, B, x (libyuv ABGR, RGBx/RGBA).
// width and height even
inline bool RgbToI420(const uint8_t* src, int stride, bool bgr, uint8_t* y, int strideY, uint8_t* u, int strideU,
                      uint8_t* v, int strideV, int width, int height)
{
    int rc = bgr ? libyuv::ARGBToI420(src, stride, y, strideY, u, strideU, v, strideV, width, height)
                 : libyuv::ABGRToI420(src, stride, y, strideY, u, strideU, v, strideV, width, height);
    if (rc != 0) return false;
    // coefficients * 65536 / 255, rows sum to 0 so grey stays exactly 128
    constexpr int kUR = -9714, kUG = -19070, kUB = 28784;
    constexpr int kVR = 28784, kVG = -24103, kVB = -4681;
    const int ri = bgr ? 2 : 0, bi = bgr ? 0 : 2;
    for (int row = 0; row < height / 2; row++) {
        const uint8_t* a = src + static_cast<ptrdiff_t>(row) * 2 * stride;
        const uint8_t* b = a + stride;
        uint8_t* uo = u + static_cast<ptrdiff_t>(row) * strideU;
        uint8_t* vo = v + static_cast<ptrdiff_t>(row) * strideV;
        for (int col = 0; col < width / 2; col++, a += 8, b += 8) {
            int r = a[ri] + a[ri + 4] + b[ri] + b[ri + 4];
            int g = a[1] + a[5] + b[1] + b[5];
            int bl = a[bi] + a[bi + 4] + b[bi] + b[bi + 4];
            // sums of 4 pixels: >> 18 = / 4 / 65536, + 128 << 18 offset, + 1 << 17 rounds
            uo[col] = static_cast<uint8_t>((kUR * r + kUG * g + kUB * bl + (128 << 18) + (1 << 17)) >> 18);
            vo[col] = static_cast<uint8_t>((kVR * r + kVG * g + kVB * bl + (128 << 18) + (1 << 17)) >> 18);
        }
    }
    return true;
}

} // namespace vv
