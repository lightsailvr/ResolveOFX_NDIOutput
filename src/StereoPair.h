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
    a starving partner eye must never freeze the stream — the pairer falls
    back to labeled mono until the partner returns.

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
// to Mono, instance gone): degrade to labeled mono rather than freeze. Must
// sit well above the pending timeout plus the observed arrival skew so a slow
// partner is never mistaken for a missing one.
constexpr uint64_t kStarvationTimeoutMs = 1500;

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
    SendMono,  // stream the submitted frame as-is, single eye
    Hold,      // held awaiting its partner; nothing to send
    SendPair,  // partner found: pack the submitted frame with matePayload
    Drop       // do not send (e.g. thumbnail render while stereo is active)
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
        // mono they stream exactly as they always have; while stereo is
        // active a tiny single-eye frame must not hijack the packed stream.
        if (isThumbnail) {
            result.action = stereoActive_ ? SubmitAction::Drop : SubmitAction::SendMono;
            return result;
        }

        // Stereo (re)activation: from mono, a right-eye render is the stereo
        // signal (the detector keys on "did an R call arrive", never on the
        // eye property's mere presence). From a fallback state, only the
        // MISSING eye returning re-activates — the still-flowing eye must
        // never re-latch, or the stream would oscillate between fallback and
        // freezing.
        if (!stereoActive_ &&
            ((fallbackEye_ < 0 && eye == kEyeRight) ||
             (fallbackEye_ >= 0 && eye != fallbackEye_))) {
            stereoActive_ = true;
            fallbackEye_ = -1;
            stereoActivatedMs_ = nowMs;
        }

        lastSeenMs_[eye == kEyeRight ? 1 : 0] = nowMs;

        if (!stereoActive_) {
            result.action = SubmitAction::SendMono;
            return result;
        }

        // Partner starvation: the other eye has been silent past the window
        // (measured from stereo activation when it was never seen at all).
        const int other = (eye == kEyeRight) ? 0 : 1;
        const uint64_t otherSeenMs =
            lastSeenMs_[other] > stereoActivatedMs_ ? lastSeenMs_[other] : stereoActivatedMs_;
        if (nowMs - otherSeenMs > kStarvationTimeoutMs) {
            stereoActive_ = false;
            fallbackEye_ = eye;
            dropped_ += pending_.size();
            for (auto& entry : pending_) {
                recycle(std::move(entry.second.payload));
            }
            pending_.clear();
            result.action = SubmitAction::SendMono;
            return result;
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
        if (stereoActive_) return StreamMode::Stereo;
        if (fallbackEye_ == kEyeLeft) return StreamMode::LeftOnly;
        if (fallbackEye_ == kEyeRight) return StreamMode::RightOnly;
        return StreamMode::Mono;
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
    bool stereoActive_ = false;
    int fallbackEye_ = -1;             // -1 = never fallen back; else the eye still flowing
    uint64_t stereoActivatedMs_ = 0;
    uint64_t lastSeenMs_[2] = {0, 0};  // [0]=left, [1]=right
    unsigned long long dropped_ = 0;
    unsigned long long holdSeq_ = 0;
};

} // namespace ndi_stereo

#endif
