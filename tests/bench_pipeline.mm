// Per-stage timing harness for the GPU fast path + NDI send pipeline (issue #5
// diagnosis: 8K stereo playback drops 30fps -> 5fps with the plugin enabled).
//
// Reproduces, outside Resolve, the exact per-pair call pattern the plugin runs
// at production dimensions (8160x7200 -> divisor 2 -> 4080x3600/eye, packed
// side-by-side 8160x3600 UYVY = 58.7 MB/frame):
//
//   left eye:  blit(no wait) + fused GPU convert (waits) -> pairer Hold ->
//              NULL async flush  <- suspects: waits out the PREVIOUS pair's
//                                   SpeedHQ encode, killing async overlap
//   right eye: blit(no wait) + fused GPU convert -> SendPair -> pack 58.7 MB ->
//              NDIlib_send_send_video_async_v2 on a clock_video=true sender
//
// Stages isolate each suspect; variants toggle exactly one of {flush,
// clock_video} at a time. An in-process receiver (fastest color format)
// mirrors NDI Video Monitor running on the same machine, so the sender does
// real encode/transport work.
//
// Build & run: make bench   (needs a Metal device + the NDI Advanced SDK;
// creates NDI source "NDI_BENCH_PIPELINE" — never the production name).
//
// Fidelity gaps vs in-host, by design: the GPU queue is idle (no Resolve
// grade work contends), and render-action/host interactions are absent. If
// every stage here is fast but Resolve still crawls, the remaining suspect is
// host interaction (waitUntilCompleted behind host GPU work) — that needs
// in-host stage timers, not this harness.

#include "MetalGPUAcceleration.h"
#include "StreamResolution.h"
#include "StereoPair.h"

#include <Processing.NDI.Lib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/sysctl.h>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

template <typename F>
static double runMs(F&& f)
{
    const auto t0 = Clock::now();
    f();
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

static void printStats(const char* label, std::vector<double> ms)
{
    if (ms.empty()) {
        std::printf("%-34s (no samples)\n", label);
        return;
    }
    std::sort(ms.begin(), ms.end());
    double sum = 0;
    for (double v : ms) sum += v;
    std::printf("%-34s n=%-3zu min=%7.2f med=%7.2f p90=%7.2f max=%7.2f mean=%7.2f ms\n",
                label, ms.size(), ms.front(), ms[ms.size() / 2],
                ms[static_cast<size_t>(ms.size() * 0.9)], ms.back(), sum / ms.size());
}

static std::string sysctlString(const char* name)
{
    char buf[256] = {0};
    size_t len = sizeof(buf) - 1;
    if (sysctlbyname(name, buf, &len, nullptr, 0) != 0) {
        return "?";
    }
    return buf;
}

// --- In-process receiver: forces the sender to do real encode/transport, the
// --- way NDI Video Monitor on this machine does during Tier 2.
struct BenchReceiver {
    NDIlib_recv_instance_t recv = nullptr;
    std::thread thread;
    std::atomic<bool> stop{false};
    std::atomic<long> framesReceived{0};

    bool start(const char* senderNameSubstring)
    {
        NDIlib_find_create_t findDesc;
        findDesc.show_local_sources = true;
        findDesc.p_groups = nullptr;
        findDesc.p_extra_ips = nullptr;
        NDIlib_find_instance_t finder = NDIlib_find_create_v2(&findDesc);
        if (!finder) {
            return false;
        }
        NDIlib_source_t source{};
        bool found = false;
        const auto deadline = Clock::now() + std::chrono::seconds(5);
        while (!found && Clock::now() < deadline) {
            NDIlib_find_wait_for_sources(finder, 500);
            uint32_t n = 0;
            const NDIlib_source_t* sources = NDIlib_find_get_current_sources(finder, &n);
            for (uint32_t i = 0; i < n; ++i) {
                if (sources[i].p_ndi_name && std::strstr(sources[i].p_ndi_name, senderNameSubstring)) {
                    source = sources[i];
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            NDIlib_find_destroy(finder);
            return false;
        }

        NDIlib_recv_create_v3_t rc;
        rc.source_to_connect_to = source;
        rc.color_format = NDIlib_recv_color_format_fastest;
        rc.bandwidth = NDIlib_recv_bandwidth_highest;
        rc.allow_video_fields = false;
        rc.p_ndi_recv_name = "NDI_BENCH_RECV";
        recv = NDIlib_recv_create_v3(&rc);
        NDIlib_find_destroy(finder); // source list stays valid until destroy — recv holds its own copy now
        if (!recv) {
            return false;
        }
        thread = std::thread([this] {
            while (!stop.load()) {
                NDIlib_video_frame_v2_t video;
                if (NDIlib_recv_capture_v3(recv, &video, nullptr, nullptr, 250) == NDIlib_frame_type_video) {
                    ++framesReceived;
                    NDIlib_recv_free_video_v2(recv, &video);
                }
            }
        });
        return true;
    }

    void shutdown()
    {
        stop = true;
        if (thread.joinable()) {
            thread.join();
        }
        if (recv) {
            NDIlib_recv_destroy(recv);
            recv = nullptr;
        }
    }
};

static NDIlib_send_instance_t createSender(bool clockVideo)
{
    NDIlib_send_create_t desc;
    desc.p_ndi_name = "NDI_BENCH_PIPELINE";
    desc.p_groups = nullptr;
    desc.clock_video = clockVideo;
    desc.clock_audio = false;
    return NDIlib_send_create(&desc);
}

static NDIlib_video_frame_v2_t makeFrame(int width, int height, uint8_t* data)
{
    NDIlib_video_frame_v2_t frame;
    frame.xres = width;
    frame.yres = height;
    frame.FourCC = NDIlib_FourCC_type_UYVY;
    frame.frame_rate_N = 30000; // matches the session under diagnosis (Frame Rate = 30)
    frame.frame_rate_D = 1000;
    frame.picture_aspect_ratio = static_cast<float>(width) / static_cast<float>(height);
    frame.frame_format_type = NDIlib_frame_format_type_progressive;
    frame.timecode = NDIlib_send_timecode_synthesize;
    frame.p_data = data;
    frame.line_stride_in_bytes = width * 2;
    frame.p_metadata = nullptr;
    return frame;
}

int main()
{
    std::printf("bench_pipeline — GPU fast path + NDI send, production dims (issue #5)\n");
    std::printf("machine: %s, %.0f GB RAM\n", sysctlString("machdep.cpu.brand_string").c_str(),
                std::stod(sysctlString("hw.memsize").empty() ? "0" : sysctlString("hw.memsize")) / 1e9);
    std::printf("NDI:     %s\n\n", NDIlib_version());

    if (!metal_gpu_is_available()) {
        std::printf("SKIP: no Metal device\n");
        return 0;
    }
    MetalGPUContextRef ctx = metal_gpu_init();
    if (!ctx) {
        std::printf("SKIP: Metal init failed\n");
        return 0;
    }
    if (!NDIlib_initialize()) {
        std::printf("SKIP: NDI runtime unavailable\n");
        return 0;
    }

    const int SRC_W = 8160, SRC_H = 7200, DIVISOR = 2;
    int outW = 0, outH = 0;
    ndi_stream::outputDims(SRC_W, SRC_H, DIVISOR, &outW, &outH); // 4080x3600

    ndi_stereo::FrameMeta eyeMeta;
    eyeMeta.width = outW;
    eyeMeta.height = outH;
    eyeMeta.format = ndi_stereo::WireFormat::UYVY8;
    int packedW = 0, packedH = 0;
    ndi_stereo::packedDims(eyeMeta, ndi_stereo::StereoLayout::SideBySide, &packedW, &packedH);
    const size_t eyeBytes = ndi_stereo::wireFrameBytes(eyeMeta);
    const size_t packedBytes = eyeBytes * 2;
    std::printf("frame:   %dx%d -> %dx%d/eye (divisor %d), packed SbS %dx%d = %.1f MB\n\n",
                SRC_W, SRC_H, outW, outH, DIVISOR, packedW, packedH, packedBytes / 1e6);

    // Source frame: row template + per-row perturbation (content the encoder
    // cannot trivially skip), same layout the host hands us (tight float RGBA).
    const size_t srcRowFloats = static_cast<size_t>(SRC_W) * 4;
    const size_t srcBytes = static_cast<size_t>(SRC_H) * srcRowFloats * sizeof(float);
    void* srcBuf = metal_gpu_create_shared_buffer(ctx, nullptr, srcBytes);
    void* blitDst = metal_gpu_create_shared_buffer(ctx, nullptr, srcBytes);
    if (!srcBuf || !blitDst) {
        std::printf("SKIP: could not allocate %.1f GB of Metal buffers\n", 2 * srcBytes / 1e9);
        return 0;
    }
    {
        std::vector<float> row(srcRowFloats);
        for (int x = 0; x < SRC_W; ++x) {
            row[x * 4 + 0] = static_cast<float>(x) / SRC_W;
            row[x * 4 + 1] = 0.5f;
            row[x * 4 + 2] = 1.0f - static_cast<float>(x) / SRC_W;
            row[x * 4 + 3] = 1.0f;
        }
        float* p = static_cast<float*>(metal_gpu_buffer_contents(srcBuf));
        for (int y = 0; y < SRC_H; ++y) {
            std::memcpy(p + y * srcRowFloats, row.data(), srcRowFloats * sizeof(float));
            p[y * srcRowFloats + 1] = static_cast<float>(y) / SRC_H;
        }
    }

    std::vector<uint8_t> uyvyL(eyeBytes), uyvyR(eyeBytes);
    std::vector<uint8_t> packed[2] = {std::vector<uint8_t>(packedBytes), std::vector<uint8_t>(packedBytes)};

    const int rowFloatsInt = static_cast<int>(srcRowFloats);
    auto convert = [&](std::vector<uint8_t>& dst) {
        metal_gpu_buffer_downscale_to_uyvy(ctx, nullptr, srcBuf, SRC_W, SRC_H, rowFloatsInt,
                                           DIVISOR, outW, outH, dst.data());
    };
    auto blitNoWait = [&] { metal_gpu_copy_buffer(ctx, nullptr, srcBuf, blitDst, srcBytes, false); };

    // --- Stage A/B/C: isolated GPU + pack costs (no NDI involved) -----------
    convert(uyvyL); // warmup: pipeline compile + first-touch
    convert(uyvyR);

    std::vector<double> t;
    for (int i = 0; i < 8; ++i) t.push_back(runMs([&] { metal_gpu_copy_buffer(ctx, nullptr, srcBuf, blitDst, srcBytes, true); }));
    printStats("A  passthrough blit 940MB (waited)", t);

    t.clear();
    for (int i = 0; i < 8; ++i) t.push_back(runMs([&] { convert(uyvyL); }));
    printStats("B  fused convert+readback div2", t);

    {
        int qW = 0, qH = 0;
        ndi_stream::outputDims(SRC_W, SRC_H, 4, &qW, &qH);
        std::vector<uint8_t> quarter(static_cast<size_t>(qW) * qH * 2);
        t.clear();
        for (int i = 0; i < 4; ++i) {
            t.push_back(runMs([&] {
                metal_gpu_buffer_downscale_to_uyvy(ctx, nullptr, srcBuf, SRC_W, SRC_H, rowFloatsInt, 4, qW, qH, quarter.data());
            }));
        }
        printStats("B4 fused convert+readback div4", t);
    }

    t.clear();
    for (int i = 0; i < 8; ++i) {
        t.push_back(runMs([&] {
            ndi_stereo::packStereoFrame(eyeMeta, ndi_stereo::StereoLayout::SideBySide,
                                        uyvyL.data(), uyvyR.data(), packed[0].data());
        }));
    }
    printStats("C  pack SbS 2x29.4MB -> 58.7MB", t);

    // --- NDI stages. Each variant toggles ONE of {flush, clock_video}. ------
    // pairLoop mirrors hubSubmitFrame's per-pair order exactly:
    //   [convL] [flush?] [convR] [pack] [async send]
    auto pairLoop = [&](NDIlib_send_instance_t sender, int pairs, bool flushPerPair,
                        std::vector<double>* flushMs, std::vector<double>* sendMs) -> double {
        int packedIndex = 0;
        bool inFlight = false;
        const auto t0 = Clock::now();
        for (int i = 0; i < pairs; ++i) {
            blitNoWait();
            convert(uyvyL);
            if (flushPerPair && inFlight) {
                const double ms = runMs([&] { NDIlib_send_send_video_async_v2(sender, nullptr); });
                inFlight = false;
                if (flushMs) flushMs->push_back(ms);
            }
            blitNoWait();
            convert(uyvyR);
            std::vector<uint8_t>& buf = packed[packedIndex ^= 1];
            ndi_stereo::packStereoFrame(eyeMeta, ndi_stereo::StereoLayout::SideBySide,
                                        uyvyL.data(), uyvyR.data(), buf.data());
            buf[(static_cast<size_t>(i) * 4096) % packedBytes] = static_cast<uint8_t>(i); // defeat identical-frame shortcuts
            NDIlib_video_frame_v2_t frame = makeFrame(packedW, packedH, buf.data());
            const double ms = runMs([&] { NDIlib_send_send_video_async_v2(sender, &frame); });
            inFlight = true;
            if (sendMs) sendMs->push_back(ms);
        }
        const double totalS = std::chrono::duration<double>(Clock::now() - t0).count();
        NDIlib_send_send_video_async_v2(sender, nullptr); // complete the last frame
        return pairs / totalS;
    };

    // Production configuration first: clock_video=true (as the plugin creates
    // its sender), receiver attached, flush-per-pair as the stereo pairer does.
    NDIlib_send_instance_t sender = createSender(/*clockVideo=*/true);
    if (!sender) {
        std::printf("SKIP: NDI sender creation failed (name in use? see LEARNINGS on leaked advertisements)\n");
        return 0;
    }
    BenchReceiver receiver;
    const bool haveReceiver = receiver.start("NDI_BENCH_PIPELINE");
    if (haveReceiver) {
        const auto deadline = Clock::now() + std::chrono::seconds(10);
        while (NDIlib_send_get_no_connections(sender, 100) < 1 && Clock::now() < deadline) {
        }
    }
    std::printf("\nreceiver: %s\n", haveReceiver ? "connected (in-process, fastest format)"
                                                 : "NOT CONNECTED — sender may skip encode; results underestimate!");

    std::vector<double> flushT, sendT;
    const double e1 = pairLoop(sender, 24, /*flushPerPair=*/true, &flushT, &sendT);
    std::printf("\nE1 production pattern (clocked, flush-per-pair): %5.1f pairs/s\n", e1);
    printStats("   E1 NULL flush wait", flushT);
    printStats("   E1 async send call", sendT);

    flushT.clear();
    sendT.clear();
    const double e2 = pairLoop(sender, 24, /*flushPerPair=*/false, nullptr, &sendT);
    std::printf("E2 no flush (async overlap preserved):           %5.1f pairs/s\n", e2);
    printStats("   E2 async send call", sendT);

    // E3: the v1.6.0 pattern — the "render thread" only enqueues the fused
    // kernel; completions land on a worker that packs pairs and sends
    // synchronously. This is what the plugin does after the async rework.
    {
        struct SimItem { void* slot; const uint8_t* p; size_t n; int eye; };
        struct Sim {
            std::mutex m;
            std::condition_variable cv;
            std::vector<SimItem> q;
            std::atomic<int> pending{0};
        };
        struct SubmitTag { Sim* sim; int eye; };
        Sim sim;
        auto onDone = [](void* user, void* slot, const void* outPtr, size_t outBytes,
                         double, bool ok) {
            SubmitTag* tag = static_cast<SubmitTag*>(user);
            if (ok) {
                std::lock_guard<std::mutex> lock(tag->sim->m);
                tag->sim->q.push_back({slot, static_cast<const uint8_t*>(outPtr), outBytes, tag->eye});
                tag->sim->cv.notify_one();
            }
            --tag->sim->pending;
            delete tag;
        };

        const int PAIRS = 40;
        std::atomic<bool> workerStop{false};
        int sent = 0, dropped = 0;
        std::thread worker([&] {
            SimItem held{};
            bool haveHeld = false;
            int packedIndex = 0;
            while (!workerStop.load()) {
                SimItem item;
                {
                    std::unique_lock<std::mutex> lock(sim.m);
                    if (!sim.cv.wait_for(lock, std::chrono::milliseconds(50),
                                         [&] { return !sim.q.empty(); })) {
                        continue;
                    }
                    item = sim.q.front();
                    sim.q.erase(sim.q.begin());
                }
                if (!haveHeld) {
                    held = item;
                    haveHeld = true;
                    continue;
                }
                const SimItem& l = (held.eye == 0) ? held : item;
                const SimItem& r = (held.eye == 0) ? item : held;
                std::vector<uint8_t>& buf = packed[packedIndex ^= 1];
                ndi_stereo::packStereoFrame(eyeMeta, ndi_stereo::StereoLayout::SideBySide,
                                            l.p, r.p, buf.data());
                NDIlib_video_frame_v2_t frame = makeFrame(packedW, packedH, buf.data());
                NDIlib_send_send_video_v2(sender, &frame); // sync, like the plugin's worker
                metal_gpu_downscale_release(ctx, held.slot);
                metal_gpu_downscale_release(ctx, item.slot);
                haveHeld = false;
                ++sent;
            }
        });

        const auto t0 = Clock::now();
        for (int i = 0; i < PAIRS; ++i) {
            for (int eye = 0; eye < 2; ++eye) {
                blitNoWait();
                SubmitTag* tag = new SubmitTag{&sim, eye};
                ++sim.pending;
                if (!metal_gpu_downscale_submit(ctx, nullptr, srcBuf, SRC_W, SRC_H,
                                                rowFloatsInt, DIVISOR, outW, outH,
                                                false, onDone, tag)) {
                    --sim.pending;
                    delete tag;
                    ++dropped;
                }
            }
            // Pace like a ~50 pairs/s host so sent-vs-dropped is meaningful;
            // the enqueue calls themselves are what must never block.
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        const double enqueueS = std::chrono::duration<double>(Clock::now() - t0).count();
        while (sim.pending.load() > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200)); // let the worker drain
        workerStop = true;
        worker.join();
        std::printf("E3 v1.6.0 pattern (enqueue-only render):         %5.1f pairs/s enqueue rate, "
                    "%d/%d pairs sent, %d eye-frames dropped\n",
                    PAIRS / enqueueS, sent, PAIRS, dropped);
    }

    // Send-only ceiling needs an UNCLOCKED sender — clock_video blocks sends
    // faster than the declared rate, hiding the encoder's true throughput.
    receiver.shutdown();
    NDIlib_send_destroy(sender);
    sender = createSender(/*clockVideo=*/false);
    BenchReceiver receiver2;
    const bool haveReceiver2 = sender && receiver2.start("NDI_BENCH_PIPELINE");
    if (haveReceiver2) {
        const auto deadline = Clock::now() + std::chrono::seconds(10);
        while (NDIlib_send_get_no_connections(sender, 100) < 1 && Clock::now() < deadline) {
        }
    }

    if (sender) {
        sendT.clear();
        int packedIndex = 0;
        const auto t0 = Clock::now();
        for (int i = 0; i < 30; ++i) {
            std::vector<uint8_t>& buf = packed[packedIndex ^= 1];
            buf[(static_cast<size_t>(i) * 8192) % packedBytes] = static_cast<uint8_t>(i);
            NDIlib_video_frame_v2_t frame = makeFrame(packedW, packedH, buf.data());
            sendT.push_back(runMs([&] { NDIlib_send_send_video_async_v2(sender, &frame); }));
        }
        const double dSeconds = std::chrono::duration<double>(Clock::now() - t0).count();
        NDIlib_send_send_video_async_v2(sender, nullptr);
        std::printf("\nD  send-only ceiling (unclocked):                %5.1f frames/s\n", 30 / dSeconds);
        printStats("   D async send call", sendT);

        flushT.clear();
        const double e1u = pairLoop(sender, 16, /*flushPerPair=*/true, &flushT, nullptr);
        std::printf("E1u production pattern, unclocked:               %5.1f pairs/s\n", e1u);
        printStats("   E1u NULL flush wait", flushT);
    }

    const long received = receiver.framesReceived.load() + receiver2.framesReceived.load();
    std::printf("\nreceiver frames captured: %ld\n", received);

    receiver2.shutdown();
    if (sender) {
        NDIlib_send_destroy(sender);
    }
    NDIlib_destroy(); // standalone process: no other senders to orphan
    metal_gpu_release_buffer(srcBuf);
    metal_gpu_release_buffer(blitDst);
    metal_gpu_shutdown(ctx);

    std::printf("\nVERDICT: production pattern sustains %.1f pairs/s (target: 30 for realtime 8K stereo)\n", e1);
    std::printf("         %s\n", e1 >= 29.0 ? "GREEN — pipeline is not the bottleneck; suspect host interaction"
                                            : "RED — the plugin pipeline itself cannot reach realtime; see stage breakdown");
    return 0;
}
