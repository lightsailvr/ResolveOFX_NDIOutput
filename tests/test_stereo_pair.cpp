// Tests for the stereo eye-pairing seam (src/StereoPair.h): the process-global
// pairing decisions (issue #6), driven by the probe findings in
// docs/2026-08-28-render-call-probe-findings.md — time-keyed pairing across
// instances, reorder tolerance, duplicate re-renders, unmated-frame drops,
// starvation fallback, and the SbS/TB plane packers.
// Build & run: make test
//
// All timing is injected (nowMs) — the pairer never reads a clock.

#include "StereoPair.h"

#include <cstdio>
#include <cstring>
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

static void expectTrue(bool actual, const char* name)
{
    expectInt(actual ? 1 : 0, 1, name);
}

using namespace ndi_stereo;

static FrameMeta meta(int w, int h, WireFormat fmt = WireFormat::UYVY8)
{
    FrameMeta m;
    m.width = w;
    m.height = h;
    m.format = fmt;
    return m;
}

// Tiny distinct payload so pair results can be byte-verified.
static std::vector<uint8_t> payload(uint8_t seed, size_t len = 8)
{
    std::vector<uint8_t> p(len);
    for (size_t i = 0; i < len; ++i) p[i] = static_cast<uint8_t>(seed + i);
    return p;
}

static SubmitResult submit(EyePairer& pairer, int eye, double time, const FrameMeta& m,
                           const std::vector<uint8_t>& bytes, uint64_t nowMs,
                           bool isThumbnail = false)
{
    return pairer.submit(eye, time, m, bytes.data(), bytes.size(), nowMs, isThumbnail);
}

int main()
{
    const FrameMeta kEyeMeta = meta(4096, 4096);

    // --- Mono timeline: left/mono frames stream immediately, are never held ---
    // (Bit-identical shipping behavior: a mono timeline must not gain latency,
    // buffering, or geometry changes. Probe: mono renders arrive as eye=L.)
    {
        EyePairer pairer;
        expectTrue(pairer.mode() == StreamMode::Mono, "mono: initial mode is Mono");

        SubmitResult r = submit(pairer, kEyeLeft, 100.0, kEyeMeta, payload(1), 0);
        expectTrue(r.action == SubmitAction::SendMono, "mono: L frame sends immediately");
        expectInt(static_cast<long long>(pairer.pendingCount()), 0, "mono: nothing held");

        // Parked re-render: the same frame time again still streams (duplicate
        // sends are today's shipping behavior when parked).
        r = submit(pairer, kEyeLeft, 100.0, kEyeMeta, payload(2), 40);
        expectTrue(r.action == SubmitAction::SendMono, "mono: parked re-render sends again");

        r = submit(pairer, kEyeLeft, 101.0, kEyeMeta, payload(3), 80);
        expectTrue(r.action == SubmitAction::SendMono, "mono: next frame sends");
        expectTrue(pairer.mode() == StreamMode::Mono, "mono: mode stays Mono");
        expectInt(static_cast<long long>(pairer.droppedFrames()), 0, "mono: nothing dropped");
    }

    // --- Stereo latch and in-order pairing (probe: at 4096²/eye R renders
    // first, L follows ~30 ms later, same time on both) ---
    {
        EyePairer pairer;

        // First right-eye render is the stereo signal (the detector keys on
        // "did an R call arrive", never on the eye property's mere presence).
        std::vector<uint8_t> rightBytes = payload(10);
        SubmitResult r = submit(pairer, kEyeRight, 200.0, kEyeMeta, rightBytes, 1000);
        expectTrue(r.action == SubmitAction::Hold, "stereo: first R is held");
        expectTrue(pairer.mode() == StreamMode::Stereo, "stereo: R latches Stereo mode");
        expectInt(static_cast<long long>(pairer.pendingCount()), 1, "stereo: one frame pending");

        // Left partner at the same time completes the pair.
        r = submit(pairer, kEyeLeft, 200.0, kEyeMeta, payload(20), 1030);
        expectTrue(r.action == SubmitAction::SendPair, "stereo: L partner completes the pair");
        expectInt(r.mateEye, kEyeRight, "stereo: mate is the right eye");
        expectTrue(r.mateMeta == kEyeMeta, "stereo: mate meta preserved");
        expectTrue(r.matePayload == rightBytes, "stereo: mate payload preserved byte-exact");
        expectInt(static_cast<long long>(pairer.pendingCount()), 0, "stereo: pair cleared pending");

        // Once stereo is active, a lone left frame holds too (no mono leak
        // mid-stereo — geometry must not flap between packed and single-eye).
        r = submit(pairer, kEyeLeft, 201.0, kEyeMeta, payload(30), 1070);
        expectTrue(r.action == SubmitAction::Hold, "stereo: L holds while awaiting R");
        r = submit(pairer, kEyeRight, 201.0, kEyeMeta, payload(31), 1090);
        expectTrue(r.action == SubmitAction::SendPair, "stereo: R completes L-led pair");
        expectInt(r.mateEye, kEyeLeft, "stereo: mate is the left eye");
        expectInt(static_cast<long long>(pairer.droppedFrames()), 0, "stereo: clean session drops nothing");
    }

    // --- Reorder window (probe at 8160×7200: either eye can lead by hundreds
    // of ms, and time goes backwards within a single eye — e.g. R rendered
    // 8202 before 8201). Pairing is keyed on time, never arrival order. ---
    {
        EyePairer pairer;
        submit(pairer, kEyeRight, 8200.0, kEyeMeta, payload(1), 0); // latches stereo

        // R runs ahead, out of order, while L lags: all pend.
        SubmitResult r = submit(pairer, kEyeRight, 8202.0, kEyeMeta, payload(2), 10);
        expectTrue(r.action == SubmitAction::Hold, "reorder: R 8202 held");
        r = submit(pairer, kEyeRight, 8201.0, kEyeMeta, payload(3), 20);
        expectTrue(r.action == SubmitAction::Hold, "reorder: R 8201 held after 8202");
        expectInt(static_cast<long long>(pairer.pendingCount()), 3, "reorder: three pending");

        // L arrives late, in its own order — each finds its partner by time.
        r = submit(pairer, kEyeLeft, 8201.0, kEyeMeta, payload(13), 200);
        expectTrue(r.action == SubmitAction::SendPair, "reorder: L 8201 pairs");
        expectTrue(r.matePayload == payload(3), "reorder: L 8201 got R 8201's payload");
        r = submit(pairer, kEyeLeft, 8202.0, kEyeMeta, payload(12), 210);
        expectTrue(r.action == SubmitAction::SendPair, "reorder: L 8202 pairs");
        expectTrue(r.matePayload == payload(2), "reorder: L 8202 got R 8202's payload");
        r = submit(pairer, kEyeLeft, 8200.0, kEyeMeta, payload(11), 220);
        expectTrue(r.action == SubmitAction::SendPair, "reorder: L 8200 pairs last");
        expectTrue(r.matePayload == payload(1), "reorder: L 8200 got R 8200's payload");
        expectInt(static_cast<long long>(pairer.pendingCount()), 0, "reorder: all pairs cleared");
    }

    // --- Parked re-renders: a duplicate same-eye submission at the same time
    // replaces the held frame (the newest render wins), and does not pair
    // with itself. ---
    {
        EyePairer pairer;
        submit(pairer, kEyeRight, 300.0, kEyeMeta, payload(40), 0);
        SubmitResult r = submit(pairer, kEyeRight, 300.0, kEyeMeta, payload(41), 30);
        expectTrue(r.action == SubmitAction::Hold, "dup: same-eye re-render holds again");
        expectInt(static_cast<long long>(pairer.pendingCount()), 1, "dup: still one pending");

        r = submit(pairer, kEyeLeft, 300.0, kEyeMeta, payload(50), 60);
        expectTrue(r.action == SubmitAction::SendPair, "dup: partner pairs");
        expectTrue(r.matePayload == payload(41), "dup: pair carries the replacement payload");
    }

    // --- Mate with mismatched meta cannot pack: the stale held frame is
    // dropped and the new frame takes its place (params settle within a
    // frame; a mismatched pair must never reach the packer). ---
    {
        EyePairer pairer;
        submit(pairer, kEyeRight, 400.0, kEyeMeta, payload(60), 0);
        SubmitResult r = submit(pairer, kEyeLeft, 400.0, meta(1920, 1080), payload(61), 30);
        expectTrue(r.action == SubmitAction::Hold, "meta: mismatched partner holds instead of pairing");
        expectInt(static_cast<long long>(pairer.droppedFrames()), 1, "meta: stale held frame counted dropped");
        expectInt(static_cast<long long>(pairer.pendingCount()), 1, "meta: new frame replaced the old");

        r = submit(pairer, kEyeRight, 400.0, meta(1920, 1080), payload(62), 60);
        expectTrue(r.action == SubmitAction::SendPair, "meta: matching partner pairs");
        expectTrue(r.matePayload == payload(61), "meta: pair carries the new-meta payload");
    }

    // --- Bounded pending: capacity cap (reorder window must cover ≥5 frames
    // per the 8K findings, but never grow without bound) — the oldest-held
    // unmated frame is evicted first. ---
    {
        EyePairer pairer;
        expectTrue(kMaxPending >= 5, "cap: window covers the probe's ≥5-frame reorder");

        for (size_t i = 0; i < kMaxPending; ++i) {
            submit(pairer, kEyeRight, 500.0 + static_cast<double>(i), kEyeMeta,
                   payload(static_cast<uint8_t>(i)), 10 * i);
        }
        expectInt(static_cast<long long>(pairer.pendingCount()),
                  static_cast<long long>(kMaxPending), "cap: window full");

        SubmitResult r = submit(pairer, kEyeRight, 600.0, kEyeMeta, payload(99),
                                10 * kMaxPending);
        expectTrue(r.action == SubmitAction::Hold, "cap: overflow frame still held");
        expectInt(static_cast<long long>(pairer.pendingCount()),
                  static_cast<long long>(kMaxPending), "cap: window stays at the cap");
        expectInt(static_cast<long long>(pairer.droppedFrames()), 1, "cap: one eviction counted");

        // Surviving (younger) slots still pair; the evicted slot was the
        // oldest-held (time 500), so its partner now finds nothing and holds.
        r = submit(pairer, kEyeLeft, 500.0 + static_cast<double>(kMaxPending - 1),
                   kEyeMeta, payload(71), 10 * kMaxPending + 10);
        expectTrue(r.action == SubmitAction::SendPair, "cap: surviving frame still pairs");
        r = submit(pairer, kEyeLeft, 500.0, kEyeMeta, payload(70), 10 * kMaxPending + 20);
        expectTrue(r.action == SubmitAction::Hold, "cap: evicted frame's partner holds");
    }

    // --- Bounded pending: age timeout — a held frame whose partner never
    // arrives is swept on a later submit instead of lingering. ---
    {
        EyePairer pairer;
        submit(pairer, kEyeRight, 700.0, kEyeMeta, payload(80), 0); // latch + hold
        // Well past the timeout, an unrelated frame arrives: the stale hold sweeps.
        SubmitResult r = submit(pairer, kEyeRight, 710.0, kEyeMeta, payload(81),
                                kPendingTimeoutMs + 1);
        expectTrue(r.action == SubmitAction::Hold, "timeout: new frame held");
        expectInt(static_cast<long long>(pairer.pendingCount()), 1, "timeout: stale frame swept");
        expectInt(static_cast<long long>(pairer.droppedFrames()), 1, "timeout: sweep counted");

        // A frame younger than the timeout is NOT swept.
        r = submit(pairer, kEyeRight, 711.0, kEyeMeta, payload(82), kPendingTimeoutMs + 500);
        expectInt(static_cast<long long>(pairer.pendingCount()), 2, "timeout: young frames survive");
    }

    // --- Single-eye starvation: when the partner eye stops rendering (e.g.
    // Vision switched back to Mono), the stream degrades to labeled mono
    // instead of freezing — and recovers to stereo when the partner returns. ---
    {
        EyePairer pairer;
        // Healthy pairing at 40 ms cadence.
        submit(pairer, kEyeRight, 900.0, kEyeMeta, payload(1), 0);
        submit(pairer, kEyeLeft, 900.0, kEyeMeta, payload(2), 30);
        expectTrue(pairer.mode() == StreamMode::Stereo, "starve: healthy stereo");

        // R stops. L keeps arriving: holds at first (inside the grace window)…
        SubmitResult r = submit(pairer, kEyeLeft, 901.0, kEyeMeta, payload(3), 70);
        expectTrue(r.action == SubmitAction::Hold, "starve: L holds inside the window");

        // …but once R has been silent past the starvation window, the stream
        // falls back to mono from the still-arriving eye. No deadlock.
        r = submit(pairer, kEyeLeft, 902.0, kEyeMeta, payload(4),
                   30 + kStarvationTimeoutMs + 1);
        expectTrue(r.action == SubmitAction::SendMono, "starve: L streams mono after window");
        expectTrue(pairer.mode() == StreamMode::LeftOnly, "starve: mode labels left-only");
        expectInt(static_cast<long long>(pairer.pendingCount()), 0,
                  "starve: fallback clears pending");

        // Subsequent L frames keep streaming mono.
        r = submit(pairer, kEyeLeft, 903.0, kEyeMeta, payload(5),
                   30 + kStarvationTimeoutMs + 40);
        expectTrue(r.action == SubmitAction::SendMono, "starve: mono keeps flowing");

        // R returns: stereo re-latches immediately and pairing resumes.
        uint64_t resumeMs = 30 + kStarvationTimeoutMs + 80;
        r = submit(pairer, kEyeRight, 904.0, kEyeMeta, payload(6), resumeMs);
        expectTrue(r.action == SubmitAction::Hold, "starve: returning R holds");
        expectTrue(pairer.mode() == StreamMode::Stereo, "starve: stereo re-latches");
        r = submit(pairer, kEyeLeft, 904.0, kEyeMeta, payload(7), resumeMs + 30);
        expectTrue(r.action == SubmitAction::SendPair, "starve: pairing resumes");
    }

    // --- Starvation with only the right eye ever arriving (left instance
    // gone): the grace window measures from stereo activation, then falls
    // back to labeled right-only mono. ---
    {
        EyePairer pairer;
        submit(pairer, kEyeRight, 1000.0, kEyeMeta, payload(1), 0); // latch, hold
        SubmitResult r = submit(pairer, kEyeRight, 1001.0, kEyeMeta, payload(2), 40);
        expectTrue(r.action == SubmitAction::Hold, "r-only: holds inside grace window");

        r = submit(pairer, kEyeRight, 1002.0, kEyeMeta, payload(3), kStarvationTimeoutMs + 1);
        expectTrue(r.action == SubmitAction::SendMono, "r-only: falls back to mono");
        expectTrue(pairer.mode() == StreamMode::RightOnly, "r-only: mode labels right-only");
    }

    // --- Thumbnail renders (filmstrip, tiny dims, probe-confirmed to hit the
    // render action): in mono they stream exactly as they always have; while
    // stereo is active they are dropped — a 184×92 frame must not hijack a
    // packed stream — and they never latch stereo or disturb pending state. ---
    {
        EyePairer pairer;
        const FrameMeta thumbMeta = meta(184, 92);

        // Mono: thumbnails keep today's shipping behavior (they stream).
        SubmitResult r = submit(pairer, kEyeLeft, 50.0, thumbMeta, payload(1), 0, true);
        expectTrue(r.action == SubmitAction::SendMono, "thumb: mono thumbnail streams as before");

        // A right-eye thumbnail must not latch stereo.
        r = submit(pairer, kEyeRight, 50.0, thumbMeta, payload(2), 10, true);
        expectTrue(pairer.mode() == StreamMode::Mono, "thumb: R thumbnail does not latch stereo");

        // Active stereo: thumbnails are dropped and leave pending untouched.
        submit(pairer, kEyeRight, 51.0, kEyeMeta, payload(3), 20);
        r = submit(pairer, kEyeLeft, 52.0, thumbMeta, payload(4), 30, true);
        expectTrue(r.action == SubmitAction::Drop, "thumb: dropped while stereo is active");
        expectInt(static_cast<long long>(pairer.pendingCount()), 1, "thumb: pending untouched");
        r = submit(pairer, kEyeLeft, 51.0, kEyeMeta, payload(5), 40);
        expectTrue(r.action == SubmitAction::SendPair, "thumb: real pairing unaffected");
    }

    // --- Frame packing: two same-meta eye frames become one packed frame.
    // Left eye goes left (SbS) / top (TB) per VR convention. Byte-exact
    // layout checks on tiny frames with distinct L/R payloads. ---
    {
        // Packed geometry and wire sizes per format.
        int w = 0, h = 0;
        packedDims(meta(4096, 4096), StereoLayout::SideBySide, &w, &h);
        expectInt(w, 8192, "pack: SbS doubles width");
        expectInt(h, 4096, "pack: SbS keeps height");
        packedDims(meta(4096, 4096), StereoLayout::TopBottom, &w, &h);
        expectInt(w, 4096, "pack: TB keeps width");
        expectInt(h, 8192, "pack: TB doubles height");

        expectInt(static_cast<long long>(wireFrameBytes(meta(2, 2, WireFormat::UYVY8))), 8,
                  "pack: UYVY frame bytes = w*h*2");
        expectInt(static_cast<long long>(wireFrameBytes(meta(2, 2, WireFormat::RGBA8))), 16,
                  "pack: RGBA frame bytes = w*h*4");
        expectInt(static_cast<long long>(wireFrameBytes(meta(2, 2, WireFormat::P216))), 16,
                  "pack: P216 frame bytes = w*h*4 (Y + interleaved UV, 16-bit)");
    }

    {
        // Single-plane format (UYVY, 2×2, rowBytes 4): SbS interleaves rows,
        // TB concatenates the frames.
        const FrameMeta m = meta(2, 2, WireFormat::UYVY8);
        std::vector<uint8_t> left = payload(0, 8);    // 0..7
        std::vector<uint8_t> right = payload(100, 8); // 100..107
        std::vector<uint8_t> out(16, 0xEE);

        packStereoFrame(m, StereoLayout::SideBySide, left.data(), right.data(), out.data());
        const uint8_t sbsExpect[16] = {0, 1, 2, 3, 100, 101, 102, 103,
                                       4, 5, 6, 7, 104, 105, 106, 107};
        expectTrue(std::memcmp(out.data(), sbsExpect, 16) == 0, "pack: UYVY SbS row layout");

        packStereoFrame(m, StereoLayout::TopBottom, left.data(), right.data(), out.data());
        const uint8_t tbExpect[16] = {0, 1, 2, 3, 4, 5, 6, 7,
                                      100, 101, 102, 103, 104, 105, 106, 107};
        expectTrue(std::memcmp(out.data(), tbExpect, 16) == 0, "pack: UYVY TB plane layout");
    }

    {
        // Planar format (P216, 2×2): each plane packs independently — the
        // packed frame is [packed Y][packed UV], never interleaved across
        // planes. Eye frame = 8 bytes Y then 8 bytes UV.
        const FrameMeta m = meta(2, 2, WireFormat::P216);
        std::vector<uint8_t> left = payload(0, 16);    // Y 0..7, UV 8..15
        std::vector<uint8_t> right = payload(100, 16); // Y 100..107, UV 108..115
        std::vector<uint8_t> out(32, 0xEE);

        packStereoFrame(m, StereoLayout::SideBySide, left.data(), right.data(), out.data());
        const uint8_t sbsExpect[32] = {
            0, 1, 2, 3, 100, 101, 102, 103,      // packed Y row 0
            4, 5, 6, 7, 104, 105, 106, 107,      // packed Y row 1
            8, 9, 10, 11, 108, 109, 110, 111,    // packed UV row 0
            12, 13, 14, 15, 112, 113, 114, 115}; // packed UV row 1
        expectTrue(std::memcmp(out.data(), sbsExpect, 32) == 0, "pack: P216 SbS per-plane layout");

        packStereoFrame(m, StereoLayout::TopBottom, left.data(), right.data(), out.data());
        const uint8_t tbExpect[32] = {
            0, 1, 2, 3, 4, 5, 6, 7,              // packed Y = L Y…
            100, 101, 102, 103, 104, 105, 106, 107, // …then R Y
            8, 9, 10, 11, 12, 13, 14, 15,        // packed UV = L UV…
            108, 109, 110, 111, 112, 113, 114, 115}; // …then R UV
        expectTrue(std::memcmp(out.data(), tbExpect, 32) == 0, "pack: P216 TB per-plane layout");
    }

    if (failures) {
        std::fprintf(stderr, "%d test(s) FAILED\n", failures);
        return 1;
    }
    std::printf("All stereo-pair tests passed\n");
    return 0;
}
