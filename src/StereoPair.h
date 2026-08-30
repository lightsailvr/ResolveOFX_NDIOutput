#ifndef _StereoPair_h_
#define _StereoPair_h_

/*
  Stereo eye-pairing seam (issue #6).

  Kept free of OFX, NDI, and Metal dependencies so it is unit-testable without
  a host (tests/test_stereo_pair.cpp). The plugin owns one EyePairer per NDI
  sender (process-global — Resolve renders each eye through its OWN plugin
  instance, so pairing can never live in instance state) and submits every
  converted wire-format frame to it; the pairer decides whether the frame
  streams as mono, waits for its partner eye, completes a pair, or is dropped.

  The decision rules encode the probe findings
  (docs/2026-08-28-render-call-probe-findings.md):
  - pairs share the frame time exactly, but arrival order is NOT guaranteed —
    either eye can lead by hundreds of ms, and times can go backwards within
    one eye, so pairing is keyed on time over a bounded reorder window;
  - parked re-renders repeat a time: a duplicate same-eye submission replaces
    the held frame;
  - unmated frames occur: pending frames age out instead of accumulating, and
    a starving partner eye must never freeze the stream.

  Geometry stability (issue #12): receivers re-fit whenever the outgoing frame
  dimensions change, which pops the picture — an acute nausea trigger for
  in-headset monitoring. So once stereo latches, the canvas is LOCKED: no
  single-eye frame is ever emitted again on the stream. A partner stall first
  repeats the last packed frame (keepalive); only sustained silence while the
  flowing eye advances through new frame times degrades the stream — to the
  flowing eye duplicated into both halves, same canvas. Recovery is damped the
  same way (a sustained, pack-compatible run from the missing eye), so
  parked-timeline phantom renders can never flap the mode in either direction.

  All timing is injected (nowMs) so behavior is deterministic under test.
*/

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

namespace ndi_stereo {

// Eye numbering follows OfxImageEye (mono renders arrive as the left eye).
constexpr int kEyeLeft = 0;
constexpr int kEyeRight = 1;

// Wire formats the send paths produce. The pairer treats payloads as opaque
// bytes; the format only has to match between mates and drive plane layout.
enum class WireFormat { UYVY8 = 0, P216 = 1, RGBA8 = 2 };

// Tuning, derived from the probe findings:
// - the reorder window must cover ≥5 frames (in-eye time reversal at 8K);
// - the pending timeout must sit comfortably above the observed ±334 ms
//   eye-arrival skew, and stay short enough that stale holds never pile up.
constexpr size_t kMaxPending = 8;
constexpr uint64_t kPendingTimeoutMs = 1000;
// Recycled hold-payload buffers kept warm (see EyePairer::recycle).
constexpr size_t kPayloadPoolCap = 4;
// A partner eye silent this long means it stopped rendering (Vision switched
// to Mono, instance gone). Degrading is a last resort — the in-headset cost of
// any mode flip is high (issue #12) — so the window sits far above the pending
// timeout, the observed ±334 ms arrival skew, and the brief stalls UI
// interactions and heavy 8K loads produce.
constexpr uint64_t kDegradeSilenceMs = 4000;
// Partner silence alone is NOT starvation: a parked timeline re-renders one
// frame time sporadically (phantom renders, see LEARNINGS pitfalls), often
// through a single eye — and there the last sent pair is already the correct
// static content. Degrading only helps when the flowing eye is advancing
// through NEW frame times its partner never matches.
constexpr int kDegradeMinDistinctTimes = 2;
// Re-latching out of degraded mode is damped the same way: the missing eye
// must sustain a run of distinct-time, meta-matching frames (gaps under
// kRecoverMaxGapMs) before the stream flips back — a lone parked phantom
// render must never flap it. At playback cadence this is still fast (~3
// frames ≈ 125 ms at 24 fps).
constexpr int kRecoverDistinctTimes = 3;
constexpr uint64_t kRecoverMaxGapMs = 1500;
// Stall keepalive: while stereo waits out the degrade window, holds whose
// partner has been quiet past this threshold re-send the last packed frame —
// receivers keep getting frames (no signal-loss flags, no frozen-stream
// ambiguity) and the geometry stays put. Sits above the probe's observed
// ±334 ms healthy eye-arrival skew so normal pairing never triggers it.
constexpr uint64_t kRepeatAfterMs = 700;

struct FrameMeta {
    int width = 0;   // one eye's dimensions, post-downscale
    int height = 0;
    WireFormat format = WireFormat::UYVY8;
};

inline bool operator==(const FrameMeta& a, const FrameMeta& b)
{
    return a.width == b.width && a.height == b.height && a.format == b.format;
}

// What the pairer currently believes the stream is. LeftOnly/RightOnly are the
// labeled degraded modes: stereo was active but the other eye starved.
enum class StreamMode { Mono, Stereo, LeftOnly, RightOnly };

enum class SubmitAction {
    SendMono,       // stream the submitted frame as-is, single eye
    Hold,           // held awaiting its partner; nothing to send
    SendPair,       // partner found: pack the submitted frame with matePayload
    SendDuplicate,  // degraded: pack the submitted frame into BOTH halves —
                    // the canvas keeps its stereo dimensions (issue #12)
    RepeatLast,     // held awaiting its partner AND the partner is stalling:
                    // re-send the last packed frame as a keepalive
    Drop            // do not send (e.g. thumbnail render while stereo is active)
};

struct SubmitResult {
    SubmitAction action = SubmitAction::SendMono;
    // SendPair only: the previously held partner frame, moved out.
    int mateEye = -1;
    FrameMeta mateMeta;
    std::vector<uint8_t> matePayload;
};

// How a completed pair is arranged in the outgoing frame. Left eye goes left
// (SideBySide) / top (TopBottom), the VR-player convention.
enum class StereoLayout { SideBySide = 0, TopBottom = 1 };

inline void packedDims(const FrameMeta& eyeMeta, StereoLayout layout, int* outWidth, int* outHeight)
{
    if (layout == StereoLayout::SideBySide) {
        *outWidth = eyeMeta.width * 2;
        *outHeight = eyeMeta.height;
    } else {
        *outWidth = eyeMeta.width;
        *outHeight = eyeMeta.height * 2;
    }
}

// Payload size of one frame of this format, matching what the send paths
// produce: UYVY interleaved 2 B/px; RGBA 4 B/px; P216 planar 16-bit Y plane
// then interleaved-UV plane (4:2:2), 2 B/px per plane.
inline size_t wireFrameBytes(const FrameMeta& m)
{
    const size_t pixels = static_cast<size_t>(m.width) * static_cast<size_t>(m.height);
    switch (m.format) {
        case WireFormat::RGBA8: return pixels * 4;
        case WireFormat::P216:  return pixels * 4;
        case WireFormat::UYVY8: default: return pixels * 2;
    }
}

// Pack two same-meta eye frames into one packed frame. `out` must hold
// 2 * wireFrameBytes(eyeMeta). Planar formats pack per plane — the packed
// frame is [packed plane 0][packed plane 1], never interleaved across planes,
// which is exactly the layout NDI expects when handed the doubled dimensions.
inline void packStereoFrame(const FrameMeta& eyeMeta, StereoLayout layout,
                            const uint8_t* left, const uint8_t* right, uint8_t* out)
{
    struct Plane { size_t rowBytes; int rows; };
    Plane planes[2];
    int planeCount = 1;
    const size_t w = static_cast<size_t>(eyeMeta.width);
    switch (eyeMeta.format) {
        case WireFormat::RGBA8:
            planes[0] = {w * 4, eyeMeta.height};
            break;
        case WireFormat::P216:
            planes[0] = {w * 2, eyeMeta.height}; // Y
            planes[1] = {w * 2, eyeMeta.height}; // interleaved UV
            planeCount = 2;
            break;
        case WireFormat::UYVY8:
        default:
            planes[0] = {w * 2, eyeMeta.height};
            break;
    }

    const uint8_t* srcL = left;
    const uint8_t* srcR = right;
    uint8_t* dst = out;
    for (int p = 0; p < planeCount; ++p) {
        const size_t rowBytes = planes[p].rowBytes;
        const int rows = planes[p].rows;
        if (layout == StereoLayout::SideBySide) {
            for (int r = 0; r < rows; ++r) {
                std::memcpy(dst, srcL + r * rowBytes, rowBytes);
                dst += rowBytes;
                std::memcpy(dst, srcR + r * rowBytes, rowBytes);
                dst += rowBytes;
            }
        } else {
            std::memcpy(dst, srcL, rowBytes * rows);
            dst += rowBytes * rows;
            std::memcpy(dst, srcR, rowBytes * rows);
            dst += rowBytes * rows;
        }
        srcL += rowBytes * rows;
        srcR += rowBytes * rows;
    }
}

class EyePairer {
public:
    SubmitResult submit(int eye, double time, const FrameMeta& meta,
                        const uint8_t* payload, size_t payloadBytes,
                        uint64_t nowMs, bool isThumbnail = false)
    {
        SubmitResult result;

        // Thumbnail renders (filmstrip/gallery) never touch pairing state: in
        // mono they stream exactly as they always have; on a locked canvas
        // (degraded included) a tiny single-eye frame must never hijack the
        // packed stream.
        if (isThumbnail) {
            result.action = canvasLocked_ ? SubmitAction::Drop : SubmitAction::SendMono;
            return result;
        }

        const int self = (eye == kEyeRight) ? 1 : 0;
        const int other = 1 - self;

        if (!canvasLocked_) {
            if (eye != kEyeRight) {
                // True mono: stream unchanged (bit-identical shipping path).
                lastSeenMs_[self] = nowMs;
                result.action = SubmitAction::SendMono;
                return result;
            }
            // Stereo activation: a right-eye render is the stereo signal (the
            // detector keys on "did an R call arrive", never on the eye
            // property's mere presence). This locks the canvas: from here on
            // every outgoing frame keeps packed-stereo dimensions — receivers
            // re-fit on a dimension change, which pops the geometry in-headset
            // (issue #12).
            canvasLocked_ = true;
            degraded_ = false;
            fallbackEye_ = -1;
            stereoActivatedMs_ = nowMs;
        }

        // Mutual-stall reset: when THIS eye returns from a silence longer
        // than the degrade window (UI stall, page switch — both eyes paused),
        // the partner's silence over that span proves nothing. Restart the
        // clock instead of degrading on the first frames back.
        if (lastSeenMs_[self] != 0 && nowMs - lastSeenMs_[self] > kDegradeSilenceMs) {
            silenceFloorMs_ = nowMs;
            resetDistinctHolds();
        }
        lastSeenMs_[self] = nowMs;

        if (degraded_) {
            if (eye == fallbackEye_) {
                // The flowing eye keeps the locked canvas alive, duplicated
                // into both halves (2D momentarily, geometry never pops).
                lastFlowingMeta_ = meta;
                notePacked(meta);
                result.action = SubmitAction::SendDuplicate;
                return result;
            }
            // The missing eye is back. Re-latching is damped: it takes a
            // sustained run of distinct-time frames that could actually pack
            // against the flowing eye (meta match) — a lone parked phantom
            // render must never flap the stream back. The frames still hold
            // below, so pairing is warm the moment the mode flips.
            if (meta == lastFlowingMeta_ &&
                (!hasRecoverTime_ || time != lastRecoverTime_)) {
                if (recoverStreak_ > 0 && nowMs - lastRecoverMs_ > kRecoverMaxGapMs) {
                    recoverStreak_ = 0; // sporadic, not sustained — start over
                }
                ++recoverStreak_;
                lastRecoverMs_ = nowMs;
                lastRecoverTime_ = time;
                hasRecoverTime_ = true;
                if (recoverStreak_ >= kRecoverDistinctTimes) {
                    degraded_ = false;
                    fallbackEye_ = -1;
                    stereoActivatedMs_ = nowMs; // fresh silence clock for both eyes
                    resetDistinctHolds();
                    resetRecovery();
                }
            }
        }

        if (!degraded_) {
            // Partner starvation, damped: the other eye must have been silent
            // past the (long) degrade window — measured from stereo activation
            // when it was never seen at all — AND the flowing eye must be
            // advancing through distinct frame times (this submit included).
            const bool advancesTime = !hasDistinctHoldTime_ || time != lastDistinctHoldTime_;
            if (nowMs - partnerSeenMs(other) > kDegradeSilenceMs &&
                distinctHoldCount_ + (advancesTime ? 1 : 0) >= kDegradeMinDistinctTimes) {
                degraded_ = true;
                fallbackEye_ = eye;
                lastFlowingMeta_ = meta;
                notePacked(meta);
                resetDistinctHolds();
                resetRecovery();
                dropped_ += pending_.size();
                for (auto& entry : pending_) {
                    recycle(std::move(entry.second.payload));
                }
                pending_.clear();
                result.action = SubmitAction::SendDuplicate;
                return result;
            }
        }

        sweepStalePending(nowMs);

        auto it = pending_.find(time);
        if (it != pending_.end() && it->second.eye != eye) {
            if (it->second.meta == meta) {
                // Partner already waiting: complete the pair.
                result.action = SubmitAction::SendPair;
                result.mateEye = it->second.eye;
                result.mateMeta = it->second.meta;
                result.matePayload = std::move(it->second.payload);
                pending_.erase(it);
                notePacked(meta);
                resetDistinctHolds(); // partner demonstrably alive
                return result;
            }
            // Mismatched partner (e.g. format/resolution param settling):
            // unpackable — drop the stale held frame, hold the new one.
            recycle(std::move(it->second.payload));
            pending_.erase(it);
            ++dropped_;
        }

        // No partner yet: hold this frame until it arrives. The hold buffer
        // comes from the recycle pool — a fresh multi-MB vector per held eye
        // costs mmap + zero-fill page faults on every pair (measured ~30 ms
        // at 8K under playback load, inside the caller's lock); warm pages
        // make it a plain memcpy.
        PendingFrame& slot = pending_[time];
        slot.eye = eye;
        slot.meta = meta;
        if (slot.payload.capacity() == 0) {
            slot.payload = takePooled();
        }
        slot.payload.assign(payload, payload + payloadBytes);
        slot.heldAtMs = nowMs;
        slot.heldSeq = ++holdSeq_;
        evictOverCapacity();

        // Degrade accounting: an unpaired hold at a NEW frame time is the
        // flowing eye advancing past its silent partner. Same-time re-holds
        // (parked phantom re-renders) deliberately don't count.
        if (!hasDistinctHoldTime_ || time != lastDistinctHoldTime_) {
            ++distinctHoldCount_;
            lastDistinctHoldTime_ = time;
            hasDistinctHoldTime_ = true;
        }

        // Stall keepalive: the partner is quiet past the repeat threshold and
        // the last packed frame still matches this stream's canvas — ask the
        // caller to re-send it. Never while degraded (the flowing eye owns
        // the keepalive there), and never before anything packed has gone out.
        if (!degraded_ && hasPacked_ && meta == lastPackedEyeMeta_ &&
            nowMs - partnerSeenMs(other) > kRepeatAfterMs) {
            result.action = SubmitAction::RepeatLast;
            return result;
        }

        result.action = SubmitAction::Hold;
        return result;
    }

    // Return a payload buffer for reuse (e.g. a consumed matePayload after
    // packing). Capacity is kept; contents are discarded.
    void recycle(std::vector<uint8_t>&& v)
    {
        if (pool_.size() < kPayloadPoolCap && v.capacity() > 0) {
            v.clear();
            pool_.push_back(std::move(v));
        }
    }

    StreamMode mode() const
    {
        if (degraded_) {
            return fallbackEye_ == kEyeLeft ? StreamMode::LeftOnly : StreamMode::RightOnly;
        }
        return canvasLocked_ ? StreamMode::Stereo : StreamMode::Mono;
    }

    size_t pendingCount() const { return pending_.size(); }
    unsigned long long droppedFrames() const { return dropped_; }

private:
    struct PendingFrame {
        int eye = kEyeLeft;
        FrameMeta meta;
        std::vector<uint8_t> payload;
        uint64_t heldAtMs = 0;
        unsigned long long heldSeq = 0; // hold order (heldAtMs can tie)
    };

    std::vector<uint8_t> takePooled()
    {
        if (pool_.empty()) {
            return {};
        }
        std::vector<uint8_t> v = std::move(pool_.back());
        pool_.pop_back();
        return v;
    }

    void resetDistinctHolds()
    {
        distinctHoldCount_ = 0;
        hasDistinctHoldTime_ = false;
    }

    void resetRecovery()
    {
        recoverStreak_ = 0;
        hasRecoverTime_ = false;
    }

    void notePacked(const FrameMeta& eyeMeta)
    {
        hasPacked_ = true;
        lastPackedEyeMeta_ = eyeMeta;
    }

    // When the partner was last submitted — floored at stereo activation (an
    // eye that never arrived counts as "last seen" at the latch) and at the
    // mutual-stall reset point.
    uint64_t partnerSeenMs(int other) const
    {
        uint64_t seen = lastSeenMs_[other] > stereoActivatedMs_ ? lastSeenMs_[other] : stereoActivatedMs_;
        return seen > silenceFloorMs_ ? seen : silenceFloorMs_;
    }

    void sweepStalePending(uint64_t nowMs)
    {
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (nowMs - it->second.heldAtMs > kPendingTimeoutMs) {
                recycle(std::move(it->second.payload));
                it = pending_.erase(it);
                ++dropped_;
            } else {
                ++it;
            }
        }
    }

    void evictOverCapacity()
    {
        while (pending_.size() > kMaxPending) {
            auto oldest = pending_.begin();
            for (auto it = pending_.begin(); it != pending_.end(); ++it) {
                if (it->second.heldSeq < oldest->second.heldSeq) {
                    oldest = it;
                }
            }
            recycle(std::move(oldest->second.payload));
            pending_.erase(oldest);
            ++dropped_;
        }
    }

    // Keyed on the exact frame time — mates share it bit-for-bit (probe-verified),
    // and exact double equality keeps fractional (retimed) times working.
    std::map<double, PendingFrame> pending_;
    std::vector<std::vector<uint8_t>> pool_; // recycled hold-payload buffers
    // Once locked (first stereo latch), the outgoing canvas keeps packed
    // dimensions for the stream's lifetime — degraded mode duplicates the
    // flowing eye instead of shrinking the frame (issue #12).
    bool canvasLocked_ = false;
    bool degraded_ = false;            // one eye starved; canvas held via duplication
    int fallbackEye_ = -1;             // the still-flowing eye while degraded
    uint64_t stereoActivatedMs_ = 0;
    uint64_t silenceFloorMs_ = 0;      // partner-silence clock floor (mutual-stall reset)
    uint64_t lastSeenMs_[2] = {0, 0};  // [0]=left, [1]=right
    int distinctHoldCount_ = 0;        // unpaired holds at distinct times (degrade gate)
    double lastDistinctHoldTime_ = 0.0;
    bool hasDistinctHoldTime_ = false;
    FrameMeta lastFlowingMeta_;        // meta of the degraded stream's flowing eye
    bool hasPacked_ = false;           // a packed frame (pair or duplicate) went out
    FrameMeta lastPackedEyeMeta_;      // per-eye meta of that packed frame
    int recoverStreak_ = 0;            // missing-eye frames counting toward re-latch
    uint64_t lastRecoverMs_ = 0;
    double lastRecoverTime_ = 0.0;
    bool hasRecoverTime_ = false;
    unsigned long long dropped_ = 0;
    unsigned long long holdSeq_ = 0;
};

} // namespace ndi_stereo

#endif
