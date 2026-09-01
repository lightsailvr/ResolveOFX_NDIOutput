// Correctness test for the CUDA GPU-native fast path
// (src/CudaGPUAcceleration.cu): the fused downscale+convert kernels must be
// BYTE-IDENTICAL to the CPU reference composition
// (ndi_stream::downscaleRGBABox + the flipping CPU converters) on a real CUDA
// device — ticket #22's bar, stricter than the Metal test's rounding
// tolerance; the module compiles with -fmad=false and mirrors the CPU
// arithmetic operation-for-operation to earn it. Also covers the passthrough
// copy, the full-frame readback used by the CPU fallback, the non-blocking
// slot ring, and the STMap warp kernels against the ndi_stmap::warpRGBABox
// CPU reference (identity map ≡ the plain downscale kernels, byte-for-byte).
// Build & run: ctest -R test_cuda_downscale (Windows with any CUDA device;
// skips cleanly without one — hosted CI compiles it but has no GPU).

#include "CudaGPUAcceleration.h"
#include "STMap.h"
#include "StreamResolution.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <mutex>
#include <vector>

static int failures = 0;

static void check(bool ok, const char* name)
{
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL %s\n", name);
    } else {
        std::printf("ok   %s\n", name);
    }
}

// Reference UYVY packing — same math as convertRGBAToUYVY_CPU in
// NDIOutputPlugin.cpp (vertical flip, Rec.709, truncating 8-bit casts),
// applied to an already-downscaled bottom-up float RGBA buffer. Kept textually
// identical to the copy in tests/test_metal_downscale.mm — both GPU modules
// are held to the same CPU oracle.
static void referenceUYVY(const float* src, int width, int height, std::vector<uint8_t>& out)
{
    out.assign(static_cast<size_t>(width) * height * 2, 0);
    for (int y = 0; y < height; ++y) {
        int srcRow = height - 1 - y;
        for (int x = 0; x < width; x += 2) {
            int srcIdx1 = (srcRow * width + x) * 4;
            int srcIdx2 = (srcRow * width + x + 1) * 4;
            int dstIdx = (y * width + x) * 2;

            float r1 = std::fmax(0.0f, std::fmin(1.0f, src[srcIdx1 + 0]));
            float g1 = std::fmax(0.0f, std::fmin(1.0f, src[srcIdx1 + 1]));
            float b1 = std::fmax(0.0f, std::fmin(1.0f, src[srcIdx1 + 2]));
            float r2 = (x + 1 < width) ? std::fmax(0.0f, std::fmin(1.0f, src[srcIdx2 + 0])) : r1;
            float g2 = (x + 1 < width) ? std::fmax(0.0f, std::fmin(1.0f, src[srcIdx2 + 1])) : g1;
            float b2 = (x + 1 < width) ? std::fmax(0.0f, std::fmin(1.0f, src[srcIdx2 + 2])) : b1;

            float y1 = 0.2126f * r1 + 0.7152f * g1 + 0.0722f * b1;
            float y2 = 0.2126f * r2 + 0.7152f * g2 + 0.0722f * b2;
            float u = -0.1146f * ((r1 + r2) * 0.5f) - 0.3854f * ((g1 + g2) * 0.5f) + 0.5f * ((b1 + b2) * 0.5f);
            float v = 0.5f * ((r1 + r2) * 0.5f) - 0.4542f * ((g1 + g2) * 0.5f) - 0.0458f * ((b1 + b2) * 0.5f);

            out[dstIdx + 0] = static_cast<uint8_t>((u + 0.5f) * 255.0f);
            out[dstIdx + 1] = static_cast<uint8_t>(y1 * 255.0f);
            out[dstIdx + 2] = static_cast<uint8_t>((v + 0.5f) * 255.0f);
            out[dstIdx + 3] = static_cast<uint8_t>(y2 * 255.0f);
        }
    }
}

// Reference P216 packing — same math as the CPU HDR path in sendHDRFrame
// (vertical flip, Rec.2020, BT.2100 limited range, planar Y + interleaved UV).
static void referenceP216(const float* src, int width, int height, std::vector<uint16_t>& out)
{
    out.assign(static_cast<size_t>(width) * height * 2, 0);
    uint16_t* yPlane = out.data();
    uint16_t* uvPlane = out.data() + static_cast<size_t>(width) * height;
    for (int y = 0; y < height; ++y) {
        int srcRow = height - 1 - y;
        for (int x = 0; x < width; x += 2) {
            int srcIdx1 = (srcRow * width + x) * 4;
            int srcIdx2 = (srcRow * width + x + 1) * 4;

            float r1 = std::fmax(0.0f, std::fmin(1.0f, src[srcIdx1 + 0]));
            float g1 = std::fmax(0.0f, std::fmin(1.0f, src[srcIdx1 + 1]));
            float b1 = std::fmax(0.0f, std::fmin(1.0f, src[srcIdx1 + 2]));
            float r2 = (x + 1 < width) ? std::fmax(0.0f, std::fmin(1.0f, src[srcIdx2 + 0])) : r1;
            float g2 = (x + 1 < width) ? std::fmax(0.0f, std::fmin(1.0f, src[srcIdx2 + 1])) : g1;
            float b2 = (x + 1 < width) ? std::fmax(0.0f, std::fmin(1.0f, src[srcIdx2 + 2])) : b1;

            float y1 = 0.2627f * r1 + 0.6780f * g1 + 0.0593f * b1;
            float y2 = 0.2627f * r2 + 0.6780f * g2 + 0.0593f * b2;
            float avgR = (r1 + r2) * 0.5f, avgG = (g1 + g2) * 0.5f, avgB = (b1 + b2) * 0.5f;
            float u = -0.1396f * avgR - 0.3604f * avgG + 0.5f * avgB;
            float v = 0.5f * avgR - 0.4598f * avgG - 0.0402f * avgB;

            int yIdx = y * width + x;
            yPlane[yIdx] = static_cast<uint16_t>(4096 + y1 * 56064);
            if (x + 1 < width) {
                yPlane[yIdx + 1] = static_cast<uint16_t>(4096 + y2 * 56064);
            }
            int uvIdx = yIdx / 2;
            uvPlane[uvIdx * 2] = static_cast<uint16_t>(32768 + u * 28672);
            uvPlane[uvIdx * 2 + 1] = static_cast<uint16_t>(32768 + v * 28672);
        }
    }
}

// Deterministic frame content: gradients across all channels, including
// out-of-range values so both paths must clamp identically.
static void fillFrame(std::vector<float>& frame, int width, int height, int rowFloats)
{
    frame.assign(static_cast<size_t>(height) * rowFloats, 999.0f); // poison padding
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float* px = frame.data() + static_cast<size_t>(y) * rowFloats + x * 4;
            px[0] = static_cast<float>(x) / (width - 1) * 1.4f - 0.2f; // sweeps past [0,1]
            px[1] = static_cast<float>(y) / (height - 1);
            px[2] = static_cast<float>((x * 7 + y * 13) % 17) / 16.0f;
            px[3] = 1.0f;
        }
    }
}

// Byte-identical or bust: any differing value is a failure, and the max diff
// is printed so a near-miss (FMA contraction, reordered sums) is diagnosable.
template <typename T>
static bool compareExact(const T* actual, const T* expected, size_t count, const char* what)
{
    size_t bad = 0;
    long maxDiff = 0;
    for (size_t i = 0; i < count; ++i) {
        long diff = static_cast<long>(actual[i]) - static_cast<long>(expected[i]);
        if (diff < 0) diff = -diff;
        if (diff > maxDiff) maxDiff = diff;
        if (diff != 0) ++bad;
    }
    if (bad) {
        std::fprintf(stderr, "  %s: %zu/%zu values differ (max diff %ld) — must be byte-identical\n",
                     what, bad, count, maxDiff);
    }
    return bad == 0;
}

int main()
{
    if (!cuda_gpu_is_available()) {
        std::printf("CUDA not available on this machine — skipping GPU kernel tests\n");
        return 0;
    }

    CudaGPUContextRef ctx = cuda_gpu_init();
    if (!ctx) {
        std::fprintf(stderr, "FAIL cuda_gpu_init\n");
        return 1;
    }

    const int srcW = 64, srcH = 32;

    struct Case { int divisor; int rowFloats; const char* name; };
    const Case cases[] = {
        {1, srcW * 4, "divisor 1, tight rows"},
        {2, srcW * 4, "divisor 2, tight rows"},
        {4, srcW * 4, "divisor 4, tight rows"},
        {2, (srcW + 2) * 4, "divisor 2, padded rows"},
    };

    for (const Case& c : cases) {
        std::vector<float> frame;
        fillFrame(frame, srcW, srcH, c.rowFloats);

        void* srcBuf = cuda_gpu_create_device_buffer(ctx, frame.data(), frame.size() * sizeof(float));
        if (!srcBuf) {
            ++failures;
            std::fprintf(stderr, "FAIL create source buffer (%s)\n", c.name);
            continue;
        }

        int outW = 0, outH = 0;
        ndi_stream::outputDims(srcW, srcH, c.divisor, &outW, &outH);

        // CPU reference: box downscale (row order preserved), then flip+convert.
        std::vector<float> small(static_cast<size_t>(outW) * outH * 4);
        ndi_stream::downscaleRGBABox(frame.data(), srcW, srcH, c.rowFloats, c.divisor,
                                     small.data(), outW, outH);

        {
            std::vector<uint8_t> expected, actual(static_cast<size_t>(outW) * outH * 2, 0);
            referenceUYVY(small.data(), outW, outH, expected);
            bool ran = cuda_gpu_buffer_downscale_to_uyvy(ctx, nullptr, srcBuf,
                                                         srcW, srcH, c.rowFloats, c.divisor,
                                                         outW, outH, actual.data());
            char name[128];
            std::snprintf(name, sizeof(name), "UYVY kernel byte-identical to CPU reference (%s)", c.name);
            check(ran && compareExact(actual.data(), expected.data(), expected.size(), name), name);
        }

        {
            std::vector<uint16_t> expected, actual(static_cast<size_t>(outW) * outH * 2, 0);
            referenceP216(small.data(), outW, outH, expected);
            bool ran = cuda_gpu_buffer_downscale_to_p216(ctx, nullptr, srcBuf,
                                                         srcW, srcH, c.rowFloats, c.divisor,
                                                         outW, outH, actual.data());
            char name[128];
            std::snprintf(name, sizeof(name), "P216 kernel byte-identical to CPU reference (%s)", c.name);
            check(ran && compareExact(actual.data(), expected.data(), expected.size(), name), name);
        }

        cuda_gpu_release_buffer(srcBuf);
    }

    // Passthrough copy: device-to-device copy must move bytes verbatim.
    // The test waits for completion before inspecting; the plugin passes
    // false and lets the host's stream ordering handle it.
    {
        std::vector<float> frame;
        fillFrame(frame, srcW, srcH, srcW * 4);
        const size_t bytes = frame.size() * sizeof(float);
        void* srcBuf = cuda_gpu_create_device_buffer(ctx, frame.data(), bytes);
        void* dstBuf = cuda_gpu_create_device_buffer(ctx, nullptr, bytes);
        std::vector<float> readback(frame.size(), -1.0f);
        bool ran = cuda_gpu_copy_buffer(ctx, nullptr, srcBuf, dstBuf, bytes, true);
        bool read = ran && cuda_gpu_read_buffer(ctx, nullptr, dstBuf, readback.data(), bytes);
        check(read && std::memcmp(readback.data(), frame.data(), bytes) == 0,
              "passthrough copy moves bytes verbatim");
        cuda_gpu_release_buffer(srcBuf);
        cuda_gpu_release_buffer(dstBuf);
    }

    // Full-frame readback (the CPU-fallback ingest) returns the source bytes.
    {
        std::vector<float> frame;
        fillFrame(frame, srcW, srcH, srcW * 4);
        const size_t bytes = frame.size() * sizeof(float);
        void* srcBuf = cuda_gpu_create_device_buffer(ctx, frame.data(), bytes);
        std::vector<float> readback(frame.size(), -1.0f);
        bool ran = cuda_gpu_read_buffer(ctx, nullptr, srcBuf, readback.data(), bytes);
        check(ran && std::memcmp(readback.data(), frame.data(), bytes) == 0,
              "read_buffer returns source bytes");
        cuda_gpu_release_buffer(srcBuf);
    }

    // Non-blocking submit path: same kernels through the slot ring +
    // completion callback. Output must match the blocking path bit-for-bit,
    // the ring must refuse a submit when every slot is held, and released
    // slots must become claimable again.
    {
        struct AsyncCapture {
            std::mutex m;
            std::condition_variable cv;
            int fired = 0;
            bool ok = false;
            std::vector<uint8_t> out;
            void* slot = nullptr;
        };
        auto onDone = [](void* user, void* slot, const void* outPtr, size_t outBytes,
                         double /*gpuMs*/, bool ok) {
            AsyncCapture* cap = static_cast<AsyncCapture*>(user);
            std::lock_guard<std::mutex> lock(cap->m);
            cap->ok = ok;
            cap->slot = slot;
            cap->out.assign(static_cast<const uint8_t*>(outPtr),
                            static_cast<const uint8_t*>(outPtr) + outBytes);
            ++cap->fired;
            cap->cv.notify_all();
        };
        auto waitFired = [](AsyncCapture& cap, int n) {
            std::unique_lock<std::mutex> lock(cap.m);
            return cap.cv.wait_for(lock, std::chrono::seconds(5), [&] { return cap.fired >= n; });
        };

        std::vector<float> frame;
        fillFrame(frame, srcW, srcH, srcW * 4);
        void* srcBuf = cuda_gpu_create_device_buffer(ctx, frame.data(), frame.size() * sizeof(float));

        int outW = 0, outH = 0;
        ndi_stream::outputDims(srcW, srcH, 2, &outW, &outH);
        std::vector<float> small(static_cast<size_t>(outW) * outH * 4);
        ndi_stream::downscaleRGBABox(frame.data(), srcW, srcH, srcW * 4, 2,
                                     small.data(), outW, outH);

        {
            std::vector<uint8_t> expected;
            referenceUYVY(small.data(), outW, outH, expected);
            AsyncCapture cap;
            bool submitted = cuda_gpu_downscale_submit(ctx, nullptr, srcBuf,
                                                       srcW, srcH, srcW * 4, 2, outW, outH,
                                                       /*p216=*/false, onDone, &cap) == CUDA_SUBMIT_OK;
            bool done = submitted && waitFired(cap, 1);
            check(done && cap.ok && cap.out.size() == expected.size() &&
                      compareExact(cap.out.data(), expected.data(), expected.size(),
                                   "async UYVY"),
                  "async submit UYVY byte-identical to CPU reference (divisor 2)");
            if (cap.slot) cuda_gpu_downscale_release(ctx, cap.slot);
        }

        {
            std::vector<uint16_t> expected;
            referenceP216(small.data(), outW, outH, expected);
            AsyncCapture cap;
            bool submitted = cuda_gpu_downscale_submit(ctx, nullptr, srcBuf,
                                                       srcW, srcH, srcW * 4, 2, outW, outH,
                                                       /*p216=*/true, onDone, &cap) == CUDA_SUBMIT_OK;
            bool done = submitted && waitFired(cap, 1);
            check(done && cap.ok && cap.out.size() == expected.size() * sizeof(uint16_t) &&
                      compareExact(reinterpret_cast<const uint16_t*>(cap.out.data()),
                                   expected.data(), expected.size(), "async P216"),
                  "async submit P216 byte-identical to CPU reference (divisor 2)");
            if (cap.slot) cuda_gpu_downscale_release(ctx, cap.slot);
        }

        {
            // Hold every slot: the 4 ring slots fill, the 5th submit refuses,
            // and releasing brings the ring back.
            AsyncCapture caps[5];
            int accepted = 0;
            for (int i = 0; i < 4; ++i) {
                if (cuda_gpu_downscale_submit(ctx, nullptr, srcBuf,
                                              srcW, srcH, srcW * 4, 2, outW, outH,
                                              false, onDone, &caps[i]) == CUDA_SUBMIT_OK) {
                    ++accepted;
                }
            }
            bool fifthRefused = cuda_gpu_downscale_submit(ctx, nullptr, srcBuf,
                                                          srcW, srcH, srcW * 4, 2, outW, outH,
                                                          false, onDone, &caps[4]) == CUDA_SUBMIT_BUSY;
            bool allFired = true;
            for (int i = 0; i < 4; ++i) {
                allFired = allFired && waitFired(caps[i], 1);
                if (caps[i].slot) cuda_gpu_downscale_release(ctx, caps[i].slot);
            }
            AsyncCapture after;
            bool afterOk = (cuda_gpu_downscale_submit(ctx, nullptr, srcBuf,
                                                      srcW, srcH, srcW * 4, 2, outW, outH,
                                                      false, onDone, &after) == CUDA_SUBMIT_OK) &&
                           waitFired(after, 1);
            if (after.slot) cuda_gpu_downscale_release(ctx, after.slot);
            check(accepted == 4 && fifthRefused && allFired && afterOk,
                  "slot ring: 4 in flight, 5th refused, released slots reusable");
        }

        cuda_gpu_release_buffer(srcBuf);
    }

    // ------------------------------------------------------------------
    // STMap warp kernels. The warp with an identity map must be
    // byte-identical to the plain downscale kernels (same taps, bilinear
    // weights degenerate to exact fetches at power-of-two dims), and an
    // arbitrary map must be byte-identical to the CPU reference warp.
    // ------------------------------------------------------------------
    {
        // Identity map at source dims, row 0 = top, bottom-left v convention.
        auto identityMapUV = [](int w, int h) {
            std::vector<float> uv(static_cast<size_t>(w) * h * 2);
            for (int row = 0; row < h; ++row) {
                const float v = (h - 1 - row + 0.5f) / static_cast<float>(h);
                for (int x = 0; x < w; ++x) {
                    uv[(static_cast<size_t>(row) * w + x) * 2 + 0] = (x + 0.5f) / static_cast<float>(w);
                    uv[(static_cast<size_t>(row) * w + x) * 2 + 1] = v;
                }
            }
            return uv;
        };

        std::vector<float> frame;
        fillFrame(frame, srcW, srcH, srcW * 4);
        void* srcBuf = cuda_gpu_create_device_buffer(ctx, frame.data(), frame.size() * sizeof(float));

        std::vector<float> ident = identityMapUV(srcW, srcH);
        void* identBuf = cuda_gpu_create_device_buffer(ctx, ident.data(), ident.size() * sizeof(float));

        for (int divisor : {1, 2}) {
            int outW = 0, outH = 0;
            ndi_stream::outputDims(srcW, srcH, divisor, &outW, &outH);
            {
                std::vector<uint8_t> plain(static_cast<size_t>(outW) * outH * 2, 0);
                std::vector<uint8_t> warped(plain.size(), 1);
                bool ranPlain = cuda_gpu_buffer_downscale_to_uyvy(ctx, nullptr, srcBuf,
                                                                  srcW, srcH, srcW * 4, divisor,
                                                                  outW, outH, plain.data());
                bool ranWarp = cuda_gpu_buffer_warp_to_uyvy(ctx, nullptr, srcBuf,
                                                            srcW, srcH, srcW * 4,
                                                            identBuf, srcW, srcH, divisor,
                                                            outW, outH, warped.data());
                char name[128];
                std::snprintf(name, sizeof(name),
                              "identity-map warp UYVY == downscale kernel (divisor %d)", divisor);
                check(ranPlain && ranWarp &&
                          std::memcmp(plain.data(), warped.data(), plain.size()) == 0, name);
            }
            {
                std::vector<uint16_t> plain(static_cast<size_t>(outW) * outH * 2, 0);
                std::vector<uint16_t> warped(plain.size(), 1);
                bool ranPlain = cuda_gpu_buffer_downscale_to_p216(ctx, nullptr, srcBuf,
                                                                  srcW, srcH, srcW * 4, divisor,
                                                                  outW, outH, plain.data());
                bool ranWarp = cuda_gpu_buffer_warp_to_p216(ctx, nullptr, srcBuf,
                                                            srcW, srcH, srcW * 4,
                                                            identBuf, srcW, srcH, divisor,
                                                            outW, outH, warped.data());
                char name[128];
                std::snprintf(name, sizeof(name),
                              "identity-map warp P216 == downscale kernel (divisor %d)", divisor);
                check(ranPlain && ranWarp &&
                          std::memcmp(plain.data(), warped.data(), plain.size() * 2) == 0, name);
            }
        }

        // Arbitrary smooth map at non-source dims, including an out-of-range
        // corner region (outside the lens circle -> black). No NaNs here —
        // matching the Metal test — the CPU seam test already pins NaN
        // behavior, and this module never enables fast-math anyway.
        const int mapW = 48, mapH = 24;
        std::vector<float> mapUV(static_cast<size_t>(mapW) * mapH * 2);
        for (int row = 0; row < mapH; ++row) {
            for (int x = 0; x < mapW; ++x) {
                const float xn = (x + 0.5f) / mapW;
                const float yn = (mapH - 1 - row + 0.5f) / mapH;
                float u = xn * xn * 0.9f + 0.05f;
                float v = 0.1f + 0.75f * yn + 0.1f * xn;
                if (x < 4 && row < 4) { u = 1.5f; v = -0.25f; } // outside the circle
                mapUV[(static_cast<size_t>(row) * mapW + x) * 2 + 0] = u;
                mapUV[(static_cast<size_t>(row) * mapW + x) * 2 + 1] = v;
            }
        }
        void* mapBuf = cuda_gpu_create_device_buffer(ctx, mapUV.data(), mapUV.size() * sizeof(float));

        struct WarpCase { int divisor; int rowFloats; const char* name; };
        const WarpCase warpCases[] = {
            {1, srcW * 4, "divisor 1, tight rows"},
            {2, srcW * 4, "divisor 2, tight rows"},
            {2, (srcW + 2) * 4, "divisor 2, padded rows"},
        };
        for (const WarpCase& c : warpCases) {
            std::vector<float> wframe;
            fillFrame(wframe, srcW, srcH, c.rowFloats);
            void* wsrcBuf = cuda_gpu_create_device_buffer(ctx, wframe.data(),
                                                          wframe.size() * sizeof(float));
            int outW = 0, outH = 0;
            ndi_stream::outputDims(mapW, mapH, c.divisor, &outW, &outH);
            std::vector<float> small(static_cast<size_t>(outW) * outH * 4);
            ndi_stmap::warpRGBABox(wframe.data(), srcW, srcH, c.rowFloats,
                                   mapUV.data(), mapW, mapH, c.divisor,
                                   small.data(), outW, outH);
            {
                std::vector<uint8_t> expected, actual(static_cast<size_t>(outW) * outH * 2, 0);
                referenceUYVY(small.data(), outW, outH, expected);
                bool ran = cuda_gpu_buffer_warp_to_uyvy(ctx, nullptr, wsrcBuf,
                                                        srcW, srcH, c.rowFloats,
                                                        mapBuf, mapW, mapH, c.divisor,
                                                        outW, outH, actual.data());
                char name[128];
                std::snprintf(name, sizeof(name), "warp UYVY byte-identical to CPU reference (%s)", c.name);
                check(ran && compareExact(actual.data(), expected.data(), expected.size(), name),
                      name);
            }
            {
                std::vector<uint16_t> expected, actual(static_cast<size_t>(outW) * outH * 2, 0);
                referenceP216(small.data(), outW, outH, expected);
                bool ran = cuda_gpu_buffer_warp_to_p216(ctx, nullptr, wsrcBuf,
                                                        srcW, srcH, c.rowFloats,
                                                        mapBuf, mapW, mapH, c.divisor,
                                                        outW, outH, actual.data());
                char name[128];
                std::snprintf(name, sizeof(name), "warp P216 byte-identical to CPU reference (%s)", c.name);
                check(ran && compareExact(actual.data(), expected.data(), expected.size(), name),
                      name);
            }
            cuda_gpu_release_buffer(wsrcBuf);
        }

        // Async warp submit: same slot ring, output bit-equal to the blocking
        // warp path.
        {
            struct AsyncCapture {
                std::mutex m;
                std::condition_variable cv;
                int fired = 0;
                bool ok = false;
                std::vector<uint8_t> out;
                void* slot = nullptr;
            };
            auto onDone = [](void* user, void* slot, const void* outPtr, size_t outBytes,
                             double /*gpuMs*/, bool ok) {
                AsyncCapture* cap = static_cast<AsyncCapture*>(user);
                std::lock_guard<std::mutex> lock(cap->m);
                cap->ok = ok;
                cap->slot = slot;
                cap->out.assign(static_cast<const uint8_t*>(outPtr),
                                static_cast<const uint8_t*>(outPtr) + outBytes);
                ++cap->fired;
                cap->cv.notify_all();
            };

            int outW = 0, outH = 0;
            ndi_stream::outputDims(mapW, mapH, 2, &outW, &outH);
            std::vector<uint8_t> blocking(static_cast<size_t>(outW) * outH * 2, 0);
            bool ranBlocking = cuda_gpu_buffer_warp_to_uyvy(ctx, nullptr, srcBuf,
                                                            srcW, srcH, srcW * 4,
                                                            mapBuf, mapW, mapH, 2,
                                                            outW, outH, blocking.data());
            AsyncCapture cap;
            bool submitted = cuda_gpu_warp_submit(ctx, nullptr, srcBuf,
                                                  srcW, srcH, srcW * 4,
                                                  mapBuf, mapW, mapH, 2, outW, outH,
                                                  /*p216=*/false, onDone, &cap) == CUDA_SUBMIT_OK;
            bool done = false;
            if (submitted) {
                std::unique_lock<std::mutex> lock(cap.m);
                done = cap.cv.wait_for(lock, std::chrono::seconds(5), [&] { return cap.fired >= 1; });
            }
            check(ranBlocking && done && cap.ok && cap.out.size() == blocking.size() &&
                      std::memcmp(cap.out.data(), blocking.data(), blocking.size()) == 0,
                  "async warp submit matches blocking warp output");
            if (cap.slot) cuda_gpu_downscale_release(ctx, cap.slot);
        }

        // A missing/undersized map buffer must refuse as INVALID (the caller
        // falls back to the CPU warp), never crash. The undersized case works
        // because the module tracks its own allocations' sizes — a raw device
        // pointer has no queryable length.
        {
            int outW = 0, outH = 0;
            ndi_stream::outputDims(mapW, mapH, 1, &outW, &outH);
            std::vector<uint8_t> outBytes(static_cast<size_t>(outW) * outH * 2, 0);
            bool nullRefused = !cuda_gpu_buffer_warp_to_uyvy(ctx, nullptr, srcBuf,
                                                             srcW, srcH, srcW * 4,
                                                             nullptr, mapW, mapH, 1,
                                                             outW, outH, outBytes.data());
            void* tiny = cuda_gpu_create_device_buffer(ctx, nullptr, 16);
            auto noopDone = [](void*, void*, const void*, size_t, double, bool) {};
            bool tinyRefused = cuda_gpu_warp_submit(ctx, nullptr, srcBuf,
                                                    srcW, srcH, srcW * 4,
                                                    tiny, mapW, mapH, 1, outW, outH,
                                                    false, noopDone, nullptr) == CUDA_SUBMIT_INVALID;
            cuda_gpu_release_buffer(tiny);
            check(nullRefused && tinyRefused, "warp refuses null/undersized map buffers as invalid");
        }

        cuda_gpu_release_buffer(mapBuf);
        cuda_gpu_release_buffer(identBuf);
        cuda_gpu_release_buffer(srcBuf);
    }

    cuda_gpu_shutdown(ctx);

    if (failures) {
        std::fprintf(stderr, "%d test(s) FAILED\n", failures);
        return 1;
    }
    std::printf("All CUDA fast-path tests passed\n");
    return 0;
}
