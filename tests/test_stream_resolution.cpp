// Tests for the stream-resolution seam (src/StreamResolution.h):
// choice-index → divisor mapping, output-dimension policy, and the CPU
// box-filter downscale used when frames arrive on the CPU.
// Build & run: make test

#include "StreamResolution.h"

#include <cstdio>
#include <vector>

static int failures = 0;

static void expectInt(long long actual, long long expected, const char* name)
{
    if (actual != expected) {
        ++failures;
        std::fprintf(stderr, "FAIL %s\n  expected: %lld\n  actual:   %lld\n", name, expected, actual);
    } else {
        std::printf("ok   %s\n", name);
    }
}

static void expectFloat(float actual, float expected, const char* name)
{
    // All test values are exactly representable, so exact compare is intended.
    if (actual != expected) {
        ++failures;
        std::fprintf(stderr, "FAIL %s\n  expected: %f\n  actual:   %f\n", name, expected, actual);
    } else {
        std::printf("ok   %s\n", name);
    }
}

int main()
{
    using namespace ndi_stream;

    // --- divisorForResolutionChoice: UI choice index → downscale divisor ---
    expectInt(divisorForResolutionChoice(0), 1, "choice 0 (Full) = 1x");
    expectInt(divisorForResolutionChoice(1), 2, "choice 1 (Half) = 2x");
    expectInt(divisorForResolutionChoice(2), 4, "choice 2 (Quarter) = 4x");
    expectInt(divisorForResolutionChoice(-1), 1, "negative choice falls back to Full");
    expectInt(divisorForResolutionChoice(3), 1, "out-of-range choice falls back to Full");

    // --- outputDims ---
    int w = 0, h = 0;

    // Full passes dimensions through untouched, odd sizes included — divisor 1
    // must never alter the shipping full-resolution behavior.
    outputDims(8160, 7200, 1, &w, &h);
    expectInt(w, 8160, "full width untouched");
    expectInt(h, 7200, "full height untouched");
    outputDims(8161, 7201, 1, &w, &h);
    expectInt(w, 8161, "full keeps odd width");
    expectInt(h, 7201, "full keeps odd height");

    // The 8K-immersive case from the probe findings (8160x7200 per eye).
    outputDims(8160, 7200, 2, &w, &h);
    expectInt(w, 4080, "8160 half width");
    expectInt(h, 3600, "7200 half height");
    outputDims(8160, 7200, 4, &w, &h);
    expectInt(w, 2040, "8160 quarter width");
    expectInt(h, 1800, "7200 quarter height");

    // VR180 4096x4096 per eye.
    outputDims(4096, 4096, 2, &w, &h);
    expectInt(w, 2048, "4096 half width");
    expectInt(h, 2048, "4096 half height");

    // Downscaled width must be even — UYVY and P216 are 4:2:2 (two pixels per
    // chroma macropixel).
    outputDims(1918, 1080, 2, &w, &h);
    expectInt(w, 958, "odd half width rounds down to even");
    expectInt(h, 540, "half height floors");

    // Degenerate tiny frame (e.g. filmstrip thumbnail): clamps, never zero.
    outputDims(2, 2, 4, &w, &h);
    expectInt(w, 2, "tiny frame width clamps to 2");
    expectInt(h, 1, "tiny frame height clamps to 1");

    // --- downscaleRGBABox: 4x2 → 2x1 at divisor 2, tightly packed ---
    {
        // Per-pixel value v; channels are (v, v/2, 1-v, alpha) so each channel
        // exercises the average independently. All values chosen so float box
        // averages are exact.
        const float v[8]     = {0.0f, 0.25f, 0.5f, 0.75f,   // row 0: A B C D
                                1.0f, 0.25f, 0.75f, 0.25f}; // row 1: E F G H
        const float alpha[8] = {1.0f, 1.0f, 1.0f, 1.0f,
                                1.0f, 0.5f, 1.0f, 1.0f};
        std::vector<float> src(4 * 2 * 4);
        for (int i = 0; i < 8; ++i) {
            src[i * 4 + 0] = v[i];
            src[i * 4 + 1] = v[i] * 0.5f;
            src[i * 4 + 2] = 1.0f - v[i];
            src[i * 4 + 3] = alpha[i];
        }
        std::vector<float> dst(2 * 1 * 4, -1.0f);
        downscaleRGBABox(src.data(), 4, 2, 4 * 4, 2, dst.data(), 2, 1);

        // out(0,0) = avg(A,B,E,F) = 1.5/4; out(1,0) = avg(C,D,G,H) = 2.25/4
        expectFloat(dst[0], 0.375f,   "box avg R, left pixel");
        expectFloat(dst[1], 0.1875f,  "box avg G, left pixel");
        expectFloat(dst[2], 0.625f,   "box avg B, left pixel");
        expectFloat(dst[3], 0.875f,   "box avg A, left pixel (one 0.5 alpha)");
        expectFloat(dst[4], 0.5625f,  "box avg R, right pixel");
        expectFloat(dst[5], 0.28125f, "box avg G, right pixel");
        expectFloat(dst[6], 0.4375f,  "box avg B, right pixel");
        expectFloat(dst[7], 1.0f,     "box avg A, right pixel");
    }

    // --- downscaleRGBABox honors row stride (padding must never leak in) ---
    {
        const int srcRowFloats = 5 * 4; // 4 pixels + 1 padding pixel per row
        std::vector<float> src(2 * srcRowFloats, 999.0f); // poison the padding
        const float v[8] = {0.0f, 0.25f, 0.5f, 0.75f,
                            1.0f, 0.25f, 0.75f, 0.25f};
        for (int y = 0; y < 2; ++y) {
            for (int x = 0; x < 4; ++x) {
                for (int c = 0; c < 4; ++c) {
                    src[y * srcRowFloats + (x * 4) + c] = v[y * 4 + x];
                }
            }
        }
        std::vector<float> dst(2 * 1 * 4, -1.0f);
        downscaleRGBABox(src.data(), 4, 2, srcRowFloats, 2, dst.data(), 2, 1);
        expectFloat(dst[0], 0.375f,  "strided box avg, left pixel");
        expectFloat(dst[4], 0.5625f, "strided box avg, right pixel");
    }

    // --- downscaleRGBABox preserves row order (no flip at this stage) ---
    {
        // 4x4 → 2x2 at divisor 2. Input rows 0-1 are all 1.0, rows 2-3 mix
        // 0.25/0.75. Output row 0 must come from input rows 0-1: a flip here
        // would swap the two output rows. (Orientation flip belongs to the
        // NDI packing step, exactly once.)
        std::vector<float> src(4 * 4 * 4);
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                float val = (y < 2) ? 1.0f : ((y == 2) ? 0.25f : 0.75f);
                for (int c = 0; c < 4; ++c) src[(y * 4 + x) * 4 + c] = val;
            }
        }
        std::vector<float> dst(2 * 2 * 4, -1.0f);
        downscaleRGBABox(src.data(), 4, 4, 4 * 4, 2, dst.data(), 2, 2);
        expectFloat(dst[0], 1.0f, "output row 0 comes from input rows 0-1");
        expectFloat(dst[(2 + 0) * 4], 0.5f, "output row 1 comes from input rows 2-3");
    }

    // --- downscaleRGBABox divisor 1 = stride-removing copy ---
    {
        const int srcRowFloats = 3 * 4; // 2 pixels + 1 padding pixel per row
        std::vector<float> src(1 * srcRowFloats, 999.0f);
        src[0] = 0.25f; src[1] = 0.5f; src[2] = 0.75f; src[3] = 1.0f;
        src[4] = 0.0f;  src[5] = 1.0f; src[6] = 0.25f; src[7] = 0.5f;
        std::vector<float> dst(2 * 1 * 4, -1.0f);
        downscaleRGBABox(src.data(), 2, 1, srcRowFloats, 1, dst.data(), 2, 1);
        expectFloat(dst[0], 0.25f, "divisor 1 copies pixel 0");
        expectFloat(dst[7], 0.5f,  "divisor 1 copies pixel 1");
    }

    // --- downscaleRGBABox clamps out-of-range sampling (edge replication) ---
    {
        // 2x2 source forced to a 2x1 output at divisor 4: the sample boxes
        // extend past the source and must clamp, not read out of bounds.
        // px0 box (cols 0-3 → {0,1,1,1}, rows 0-3 → {0,1,1,1}):
        //   (0.0 + 3*0.25 + 3*(0.5 + 3*0.75)) / 16 = 9.0/16 = 0.5625
        // px1 box (cols 4-7 → all col 1): (4*0.25 + 12*0.75) / 16 = 10.0/16
        std::vector<float> src(2 * 2 * 4);
        const float vals[4] = {0.0f, 0.25f, 0.5f, 0.75f};
        for (int i = 0; i < 4; ++i)
            for (int c = 0; c < 4; ++c) src[i * 4 + c] = vals[i];
        std::vector<float> dst(2 * 1 * 4, -1.0f);
        downscaleRGBABox(src.data(), 2, 2, 2 * 4, 4, dst.data(), 2, 1);
        expectFloat(dst[0], 0.5625f, "clamped sampling, left pixel");
        expectFloat(dst[4], 0.625f,  "clamped sampling, right pixel");
    }

    if (failures) {
        std::fprintf(stderr, "%d test(s) FAILED\n", failures);
        return 1;
    }
    std::printf("All stream-resolution tests passed\n");
    return 0;
}
