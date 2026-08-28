// Correctness test for the GPU-native fast path (src/MetalGPUAcceleration.mm):
// the fused downscale+convert kernels must match the CPU reference composition
// (ndi_stream::downscaleRGBABox + the flipping CPU converters) within rounding
// tolerance, on a real Metal device. Also covers the passthrough copy and the
// full-frame readback used by the CPU fallback.
// Build & run: make test-metal (macOS with any Metal device; skips cleanly without one)

#include "MetalGPUAcceleration.h"
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
// applied to an already-downscaled bottom-up float RGBA buffer.
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

template <typename T>
static bool compareBuffers(const T* actual, const T* expected, size_t count, long tolerance,
                           const char* what)
{
    size_t bad = 0;
    long maxDiff = 0;
    for (size_t i = 0; i < count; ++i) {
        long diff = static_cast<long>(actual[i]) - static_cast<long>(expected[i]);
        if (diff < 0) diff = -diff;
        if (diff > maxDiff) maxDiff = diff;
        if (diff > tolerance) ++bad;
    }
    if (bad) {
        std::fprintf(stderr, "  %s: %zu/%zu values differ by more than %ld (max diff %ld)\n",
                     what, bad, count, tolerance, maxDiff);
    }
    return bad == 0;
}

int main()
{
    if (!metal_gpu_is_available()) {
        std::printf("Metal not available on this machine — skipping GPU kernel tests\n");
        return 0;
    }

    MetalGPUContextRef ctx = metal_gpu_init();
    if (!ctx) {
        std::fprintf(stderr, "FAIL metal_gpu_init\n");
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

        void* srcBuf = metal_gpu_create_shared_buffer(ctx, frame.data(), frame.size() * sizeof(float));
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
            bool ran = metal_gpu_buffer_downscale_to_uyvy(ctx, nullptr, srcBuf,
                                                          srcW, srcH, c.rowFloats, c.divisor,
                                                          outW, outH, actual.data());
            char name[128];
            std::snprintf(name, sizeof(name), "UYVY kernel matches CPU reference (%s)", c.name);
            check(ran && compareBuffers(actual.data(), expected.data(), expected.size(), 2, name), name);
        }

        {
            std::vector<uint16_t> expected, actual(static_cast<size_t>(outW) * outH * 2, 0);
            referenceP216(small.data(), outW, outH, expected);
            bool ran = metal_gpu_buffer_downscale_to_p216(ctx, nullptr, srcBuf,
                                                          srcW, srcH, c.rowFloats, c.divisor,
                                                          outW, outH, actual.data());
            char name[128];
            std::snprintf(name, sizeof(name), "P216 kernel matches CPU reference (%s)", c.name);
            check(ran && compareBuffers(actual.data(), expected.data(), expected.size(), 64, name), name);
        }

        metal_gpu_release_buffer(srcBuf);
    }

    // Passthrough copy: device-to-device blit must move bytes verbatim.
    // The test waits for completion before inspecting; the plugin passes
    // false and lets the host's queue ordering handle it.
    {
        std::vector<float> frame;
        fillFrame(frame, srcW, srcH, srcW * 4);
        const size_t bytes = frame.size() * sizeof(float);
        void* srcBuf = metal_gpu_create_shared_buffer(ctx, frame.data(), bytes);
        void* dstBuf = metal_gpu_create_shared_buffer(ctx, nullptr, bytes);
        bool ran = metal_gpu_copy_buffer(ctx, nullptr, srcBuf, dstBuf, bytes, true);
        check(ran && std::memcmp(metal_gpu_buffer_contents(dstBuf), frame.data(), bytes) == 0,
              "passthrough copy moves bytes verbatim");
        metal_gpu_release_buffer(srcBuf);
        metal_gpu_release_buffer(dstBuf);
    }

    // Full-frame readback (the CPU-fallback ingest) returns the source bytes.
    {
        std::vector<float> frame;
        fillFrame(frame, srcW, srcH, srcW * 4);
        const size_t bytes = frame.size() * sizeof(float);
        void* srcBuf = metal_gpu_create_shared_buffer(ctx, frame.data(), bytes);
        std::vector<float> readback(frame.size(), -1.0f);
        bool ran = metal_gpu_read_buffer(ctx, nullptr, srcBuf, readback.data(), bytes);
        check(ran && std::memcmp(readback.data(), frame.data(), bytes) == 0,
              "read_buffer returns source bytes");
        metal_gpu_release_buffer(srcBuf);
    }

    // Non-blocking submit path (v1.6.0): same kernels through the slot ring +
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
        void* srcBuf = metal_gpu_create_shared_buffer(ctx, frame.data(), frame.size() * sizeof(float));

        int outW = 0, outH = 0;
        ndi_stream::outputDims(srcW, srcH, 2, &outW, &outH);
        std::vector<float> small(static_cast<size_t>(outW) * outH * 4);
        ndi_stream::downscaleRGBABox(frame.data(), srcW, srcH, srcW * 4, 2,
                                     small.data(), outW, outH);

        {
            std::vector<uint8_t> expected;
            referenceUYVY(small.data(), outW, outH, expected);
            AsyncCapture cap;
            bool submitted = metal_gpu_downscale_submit(ctx, nullptr, srcBuf,
                                                        srcW, srcH, srcW * 4, 2, outW, outH,
                                                        /*p216=*/false, onDone, &cap);
            bool done = submitted && waitFired(cap, 1);
            check(done && cap.ok && cap.out.size() == expected.size() &&
                      compareBuffers(cap.out.data(), expected.data(), expected.size(), 2,
                                     "async UYVY"),
                  "async submit UYVY matches CPU reference (divisor 2)");
            if (cap.slot) metal_gpu_downscale_release(ctx, cap.slot);
        }

        {
            std::vector<uint16_t> expected;
            referenceP216(small.data(), outW, outH, expected);
            AsyncCapture cap;
            bool submitted = metal_gpu_downscale_submit(ctx, nullptr, srcBuf,
                                                        srcW, srcH, srcW * 4, 2, outW, outH,
                                                        /*p216=*/true, onDone, &cap);
            bool done = submitted && waitFired(cap, 1);
            check(done && cap.ok && cap.out.size() == expected.size() * sizeof(uint16_t) &&
                      compareBuffers(reinterpret_cast<const uint16_t*>(cap.out.data()),
                                     expected.data(), expected.size(), 64, "async P216"),
                  "async submit P216 matches CPU reference (divisor 2)");
            if (cap.slot) metal_gpu_downscale_release(ctx, cap.slot);
        }

        {
            // Hold every slot: the 4 ring slots fill, the 5th submit refuses,
            // and releasing brings the ring back.
            AsyncCapture caps[5];
            int accepted = 0;
            for (int i = 0; i < 4; ++i) {
                if (metal_gpu_downscale_submit(ctx, nullptr, srcBuf,
                                               srcW, srcH, srcW * 4, 2, outW, outH,
                                               false, onDone, &caps[i])) {
                    ++accepted;
                }
            }
            bool fifthRefused = !metal_gpu_downscale_submit(ctx, nullptr, srcBuf,
                                                            srcW, srcH, srcW * 4, 2, outW, outH,
                                                            false, onDone, &caps[4]);
            bool allFired = true;
            for (int i = 0; i < 4; ++i) {
                allFired = allFired && waitFired(caps[i], 1);
                if (caps[i].slot) metal_gpu_downscale_release(ctx, caps[i].slot);
            }
            AsyncCapture after;
            bool afterOk = metal_gpu_downscale_submit(ctx, nullptr, srcBuf,
                                                      srcW, srcH, srcW * 4, 2, outW, outH,
                                                      false, onDone, &after) &&
                           waitFired(after, 1);
            if (after.slot) metal_gpu_downscale_release(ctx, after.slot);
            check(accepted == 4 && fifthRefused && allFired && afterOk,
                  "slot ring: 4 in flight, 5th refused, released slots reusable");
        }

        metal_gpu_release_buffer(srcBuf);
    }

    metal_gpu_shutdown(ctx);

    if (failures) {
        std::fprintf(stderr, "%d test(s) FAILED\n", failures);
        return 1;
    }
    std::printf("All Metal fast-path tests passed\n");
    return 0;
}
