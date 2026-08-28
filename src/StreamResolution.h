#ifndef _StreamResolution_h_
#define _StreamResolution_h_

/*
  Stream-resolution seam for the Full/Half/Quarter NDI downscale (issue #5).

  Kept free of OFX, NDI, and Metal dependencies so it is unit-testable without
  a host (tests/test_stream_resolution.cpp). The plugin maps the Resolution
  choice parameter through divisorForResolutionChoice(), sizes the outgoing
  stream with outputDims(), and — on the CPU path only — downscales with
  downscaleRGBABox(). The Metal path implements the same box filter in a
  fused downscale+convert kernel (src/MetalGPUAcceleration.mm), validated
  against this reference by `make test-metal`.
*/

#include <algorithm>

namespace ndi_stream {

// Resolution choice-param index (0 = Full, 1 = Half, 2 = Quarter) → downscale
// divisor. Anything unexpected falls back to Full so a bad value can never
// distort the stream.
inline int divisorForResolutionChoice(int choiceIndex)
{
    switch (choiceIndex) {
        case 1: return 2;
        case 2: return 4;
        default: return 1;
    }
}

// Outgoing stream dimensions for a divisor. Divisor 1 passes the source
// dimensions through untouched (the shipping full-resolution behavior).
// Downscaled widths round down to even — UYVY and P216 are 4:2:2, two pixels
// per chroma macropixel — and both axes clamp so a degenerate source (e.g. a
// filmstrip thumbnail) can never produce a zero-sized frame.
inline void outputDims(int srcWidth, int srcHeight, int divisor, int* outWidth, int* outHeight)
{
    if (divisor <= 1) {
        *outWidth = srcWidth;
        *outHeight = srcHeight;
        return;
    }
    *outWidth = std::max(2, (srcWidth / divisor) & ~1);
    *outHeight = std::max(1, srcHeight / divisor);
}

// Box-filter downscale of float RGBA. Each output pixel averages the
// divisor×divisor source block starting at (x*divisor, y*divisor); samples
// falling outside the source clamp to the edge. Row order is preserved — the
// vertical flip for NDI belongs to the packing step, exactly once. srcRowFloats
// is the source stride in floats (rowBytes / sizeof(float)); dst is tightly
// packed outWidth*outHeight*4 floats. Divisor 1 degenerates to a
// stride-removing copy.
inline void downscaleRGBABox(const float* src, int srcWidth, int srcHeight, int srcRowFloats,
                             int divisor, float* dst, int outWidth, int outHeight)
{
    const float invCount = 1.0f / static_cast<float>(divisor * divisor);
    for (int y = 0; y < outHeight; ++y) {
        for (int x = 0; x < outWidth; ++x) {
            float sum[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            for (int sy = 0; sy < divisor; ++sy) {
                const int srcY = std::min(y * divisor + sy, srcHeight - 1);
                const float* row = src + static_cast<long long>(srcY) * srcRowFloats;
                for (int sx = 0; sx < divisor; ++sx) {
                    const int srcX = std::min(x * divisor + sx, srcWidth - 1);
                    const float* px = row + srcX * 4;
                    sum[0] += px[0];
                    sum[1] += px[1];
                    sum[2] += px[2];
                    sum[3] += px[3];
                }
            }
            float* out = dst + (static_cast<long long>(y) * outWidth + x) * 4;
            out[0] = sum[0] * invCount;
            out[1] = sum[1] * invCount;
            out[2] = sum[2] * invCount;
            out[3] = sum[3] * invCount;
        }
    }
}

} // namespace ndi_stream

#endif
