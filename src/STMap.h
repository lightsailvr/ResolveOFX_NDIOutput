#ifndef _STMap_h_
#define _STMap_h_

/*
  STMap projection seam (issue #7): fisheye/lens-space → equirect on the
  sender, driven by Fusion-authored per-eye EXR STMaps (the VR.NDI lens
  pipeline's Apple-independent path).

  Kept free of OFX, NDI, and Metal dependencies so it is unit-testable without
  a host (tests/test_stmap.cpp). Two pieces live here:

  - loadSTMapEXR(): a minimal, fully bounds-checked reader for the EXR subset
    STMaps are authored in — single-part scanline images, FLOAT or HALF
    channels, None/RLE/ZIPS/ZIP compression (ZIP inflates via zlib). R is the
    normalized source U, G the normalized source V (Fusion/Nuke STMap
    convention: bottom-left origin, v=0 = source bottom). Anything else —
    tiled, deep, multipart, PIZ/DWA, missing channels, truncated or hostile
    bytes — fails soft with an error string and never crashes: the plugin's
    contract is "invalid STMap = passthrough plus a status message".

  - warpRGBABox(): the CPU reference warp. For each output pixel it averages
    divisor×divisor sub-taps (the same box the plain downscale uses); each
    sub-tap reads the map texel for that full-res destination pixel and
    bilinearly samples the source there. Map texels outside [0,1] (or NaN)
    contribute transparent black — those are the "outside the lens image
    circle" regions of a fisheye map. Row order is preserved (bottom-up in,
    bottom-up out, map stored top-down as EXR files are); the vertical flip
    for NDI belongs to the packing step, exactly once. By construction
    warp-at-divisor-d equals warp-at-map-resolution followed by
    ndi_stream::downscaleRGBABox — the identity the Metal warp kernels
    (src/MetalGPUAcceleration.mm) implement and `make test-metal` checks.

  The warped output frame's dimensions are the MAP's dimensions (then the
  Resolution divisor applies): the STMap defines the destination image.
*/

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include <zlib.h>

#include "PlatformPaths.h"

namespace ndi_stmap {

// Caps applied before any allocation so a hostile header can't OOM the host.
// 1<<26 texels admits the 8160×7200 Apple-Immersive full-frame case (58.7M).
constexpr int kMaxMapDim = 16384;
constexpr long long kMaxMapTexels = 1LL << 26;
constexpr int kMaxChannels = 64;
// 1LL, not 1L: long is 32-bit on Windows, where 1L << 31 goes negative and
// would reject every file as unreadable.
constexpr long long kMaxFileBytes = 1LL << 31;

struct STMapImage {
    int width = 0;
    int height = 0;
    // Interleaved (u,v) per texel, row 0 = TOP (EXR storage order); u,v use
    // the bottom-left source convention (v=0 = source bottom row).
    std::vector<float> uv;
};

namespace detail {

struct Cursor {
    const uint8_t* p = nullptr;
    size_t n = 0;
    size_t off = 0;

    bool read(void* dst, size_t len)
    {
        if (len > n - off) return false;
        std::memcpy(dst, p + off, len);
        off += len;
        return true;
    }
    bool readU8(uint8_t* v) { return read(v, 1); }
    bool readI32(int32_t* v) { return read(v, 4); } // little-endian host assumed (as elsewhere)
    bool readU64(uint64_t* v) { return read(v, 8); }
    bool readString(std::string* s, size_t cap = 255)
    {
        s->clear();
        while (s->size() <= cap) {
            uint8_t c = 0;
            if (!readU8(&c)) return false;
            if (c == 0) return true;
            s->push_back(static_cast<char>(c));
        }
        return false;
    }
    bool skip(size_t len)
    {
        if (len > n - off) return false;
        off += len;
        return true;
    }
};

inline float halfToFloat(uint16_t h)
{
    const uint32_t sign = static_cast<uint32_t>(h >> 15) << 31;
    uint32_t exp = (h >> 10) & 0x1F;
    uint32_t man = h & 0x3FF;
    uint32_t bits;
    if (exp == 0) {
        if (man == 0) {
            bits = sign; // ±0
        } else {
            exp = 127 - 15 + 1;
            while (!(man & 0x400)) { // normalize the subnormal
                man <<= 1;
                --exp;
            }
            man &= 0x3FF;
            bits = sign | (exp << 23) | (man << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | (man << 13); // Inf/NaN
    } else {
        bits = sign | ((exp - 15 + 127) << 23) | (man << 13);
    }
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

inline bool inflateExact(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstLen)
{
    uLongf outLen = static_cast<uLongf>(dstLen);
    if (uncompress(dst, &outLen, src, static_cast<uLong>(srcLen)) != Z_OK) return false;
    return outLen == dstLen;
}

inline bool rleDecode(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstLen)
{
    size_t si = 0, di = 0;
    while (si < srcLen) {
        const int8_t c = static_cast<int8_t>(src[si++]);
        if (c < 0) {
            const size_t count = static_cast<size_t>(-static_cast<int>(c));
            if (count > srcLen - si || count > dstLen - di) return false;
            std::memcpy(dst + di, src + si, count);
            si += count;
            di += count;
        } else {
            const size_t count = static_cast<size_t>(c) + 1;
            if (si >= srcLen || count > dstLen - di) return false;
            std::memset(dst + di, src[si++], count);
            di += count;
        }
    }
    return di == dstLen;
}

// OpenEXR's post-decode reconstruction for ZIP and RLE chunks: undo the
// byte delta (+128), then re-interleave the two halves.
inline void unfilter(std::vector<uint8_t>& buf)
{
    for (size_t i = 1; i < buf.size(); ++i) {
        buf[i] = static_cast<uint8_t>(buf[i] + buf[i - 1] - 128);
    }
    std::vector<uint8_t> out(buf.size());
    const size_t half = (buf.size() + 1) / 2;
    size_t i1 = 0, i2 = half;
    for (size_t o = 0; o < out.size(); ++o) {
        out[o] = (o & 1) ? buf[i2++] : buf[i1++];
    }
    buf.swap(out);
}

struct ExrChannel {
    std::string name;
    int32_t pixelType = 0;   // 0=UINT, 1=HALF, 2=FLOAT
    size_t bytesPerPixel = 0;
    size_t lineOffset = 0;   // byte offset of this channel's run within one scanline
};

} // namespace detail

// Load an EXR STMap. Returns true and fills *out on success; on any failure
// returns false with a human-readable *error and never throws or crashes.
inline bool loadSTMapEXR(const char* path, STMapImage* out, std::string* error)
{
    const auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        return false;
    };

    std::FILE* f = ndi_path::fopenUtf8(path, "rb");
    if (!f) {
        return fail(std::string("cannot open '") + path + "'");
    }
    std::vector<uint8_t> bytes;
    bool readOk = false;
    try {
        std::fseek(f, 0, SEEK_END);
        const long fileSize = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (fileSize > 0 && fileSize <= kMaxFileBytes) {
            bytes.resize(static_cast<size_t>(fileSize));
            readOk = (std::fread(bytes.data(), 1, bytes.size(), f) == bytes.size());
        }
    } catch (const std::bad_alloc&) {
        std::fclose(f);
        return fail("out of memory reading STMap file");
    }
    std::fclose(f);
    if (!readOk) {
        return fail(std::string("cannot read '") + path + "' (empty, unreadable, or over 2 GiB)");
    }

    try {
        detail::Cursor cur{bytes.data(), bytes.size()};

        uint8_t magic[4] = {0, 0, 0, 0};
        if (!cur.read(magic, 4) || magic[0] != 0x76 || magic[1] != 0x2f ||
            magic[2] != 0x31 || magic[3] != 0x01) {
            return fail("not an EXR file (bad magic)");
        }
        int32_t version = 0;
        if (!cur.readI32(&version)) return fail("truncated EXR (no version field)");
        if ((version & 0xFF) != 2) return fail("unsupported EXR version");
        if (version & 0x200) return fail("tiled EXR unsupported (need a scanline STMap)");
        if (version & 0x800) return fail("deep EXR unsupported");
        if (version & 0x1000) return fail("multipart EXR unsupported");

        std::vector<detail::ExrChannel> channels;
        int32_t compression = -1;
        int32_t xMin = 0, yMin = 0, xMax = -1, yMax = -1;
        bool haveChannels = false, haveDataWindow = false;

        for (;;) {
            std::string attrName;
            if (!cur.readString(&attrName)) return fail("truncated EXR header");
            if (attrName.empty()) break; // end of header
            std::string attrType;
            int32_t attrSize = 0;
            if (!cur.readString(&attrType) || !cur.readI32(&attrSize) || attrSize < 0 ||
                static_cast<size_t>(attrSize) > cur.n - cur.off) {
                return fail("truncated EXR header attribute '" + attrName + "'");
            }
            const size_t attrEnd = cur.off + static_cast<size_t>(attrSize);

            if (attrName == "channels" && attrType == "chlist") {
                for (;;) {
                    detail::ExrChannel ch;
                    // list ends with a single 0 byte where the next name starts
                    if (!cur.readString(&ch.name)) return fail("truncated EXR channel list");
                    if (ch.name.empty()) break;
                    uint8_t linRes[4];
                    int32_t xSamp = 0, ySamp = 0;
                    if (!cur.readI32(&ch.pixelType) || !cur.read(linRes, 4) ||
                        !cur.readI32(&xSamp) || !cur.readI32(&ySamp)) {
                        return fail("truncated EXR channel list");
                    }
                    if (xSamp != 1 || ySamp != 1) {
                        return fail("subsampled EXR channels unsupported");
                    }
                    if (ch.pixelType < 0 || ch.pixelType > 2) {
                        return fail("unknown EXR channel pixel type");
                    }
                    ch.bytesPerPixel = (ch.pixelType == 1) ? 2 : 4; // UINT and FLOAT are 4
                    channels.push_back(ch);
                    if (channels.size() > static_cast<size_t>(kMaxChannels)) {
                        return fail("too many EXR channels");
                    }
                }
                haveChannels = true;
            } else if (attrName == "compression" && attrType == "compression") {
                uint8_t c = 255;
                if (!cur.readU8(&c)) return fail("truncated EXR compression attribute");
                compression = c;
            } else if (attrName == "dataWindow" && attrType == "box2i") {
                if (!cur.readI32(&xMin) || !cur.readI32(&yMin) ||
                    !cur.readI32(&xMax) || !cur.readI32(&yMax)) {
                    return fail("truncated EXR dataWindow attribute");
                }
                haveDataWindow = true;
            }
            // Skip whatever the branch above didn't consume (unknown
            // attributes entirely; known ones any trailing bytes).
            if (cur.off > attrEnd || !cur.skip(attrEnd - cur.off)) {
                return fail("malformed EXR header attribute '" + attrName + "'");
            }
        }

        if (!haveChannels || !haveDataWindow || compression < 0) {
            return fail("EXR header is missing channels, dataWindow, or compression");
        }
        // NONE=0, RLE=1, ZIPS=2, ZIP=3; everything else needs a real EXR library.
        if (compression > 3) {
            return fail("unsupported EXR compression (STMaps need None, RLE, or Zip)");
        }

        const long long wll = static_cast<long long>(xMax) - xMin + 1;
        const long long hll = static_cast<long long>(yMax) - yMin + 1;
        if (wll < 1 || hll < 1 || wll > kMaxMapDim || hll > kMaxMapDim ||
            wll * hll > kMaxMapTexels) {
            return fail("EXR dataWindow dimensions out of range for an STMap");
        }
        const int width = static_cast<int>(wll);
        const int height = static_cast<int>(hll);

        size_t lineBytes = 0;
        int rIndex = -1, gIndex = -1;
        for (size_t i = 0; i < channels.size(); ++i) {
            channels[i].lineOffset = lineBytes;
            lineBytes += static_cast<size_t>(width) * channels[i].bytesPerPixel;
            if (channels[i].name == "R" && rIndex < 0) rIndex = static_cast<int>(i);
            if (channels[i].name == "G" && gIndex < 0) gIndex = static_cast<int>(i);
        }
        if (rIndex < 0 || gIndex < 0) {
            return fail("STMap needs R (source U) and G (source V) channels");
        }
        if (channels[rIndex].pixelType == 0 || channels[gIndex].pixelType == 0) {
            return fail("STMap R/G channels must be float or half, not UINT");
        }

        const int linesPerBlock = (compression == 3) ? 16 : 1;
        const int chunkCount = (height + linesPerBlock - 1) / linesPerBlock;

        std::vector<uint64_t> offsets(static_cast<size_t>(chunkCount));
        for (int i = 0; i < chunkCount; ++i) {
            if (!cur.readU64(&offsets[static_cast<size_t>(i)])) {
                return fail("truncated EXR scanline offset table");
            }
        }

        out->width = width;
        out->height = height;
        out->uv.assign(static_cast<size_t>(width) * height * 2, 0.0f);
        std::vector<uint8_t> decoded;
        std::vector<bool> rowWritten(static_cast<size_t>(height), false);

        for (int ci = 0; ci < chunkCount; ++ci) {
            const uint64_t off = offsets[static_cast<size_t>(ci)];
            if (off > bytes.size() || bytes.size() - off < 8) {
                return fail("EXR chunk offset out of range");
            }
            detail::Cursor chunk{bytes.data(), bytes.size(), static_cast<size_t>(off)};
            int32_t y = 0, dataSize = 0;
            chunk.readI32(&y);
            chunk.readI32(&dataSize);
            if (y < yMin || y > yMax || dataSize <= 0 ||
                static_cast<size_t>(dataSize) > chunk.n - chunk.off) {
                return fail("malformed EXR chunk header");
            }
            const int r0 = y - yMin;
            const int linesInChunk = std::min(linesPerBlock, height - r0);
            const size_t expectedRaw = static_cast<size_t>(linesInChunk) * lineBytes;
            const uint8_t* rawData = nullptr;

            if (compression == 0 || static_cast<size_t>(dataSize) == expectedRaw) {
                // Uncompressed — either by mode, or the raw fallback OpenEXR
                // writers use when compressing wouldn't shrink the chunk
                // (stored plain, no delta/interleave filter).
                if (static_cast<size_t>(dataSize) != expectedRaw) {
                    return fail("EXR chunk size mismatch");
                }
                rawData = bytes.data() + chunk.off;
            } else {
                decoded.assign(expectedRaw, 0);
                const uint8_t* src = bytes.data() + chunk.off;
                const bool ok = (compression == 1)
                                    ? detail::rleDecode(src, static_cast<size_t>(dataSize),
                                                        decoded.data(), expectedRaw)
                                    : detail::inflateExact(src, static_cast<size_t>(dataSize),
                                                           decoded.data(), expectedRaw);
                if (!ok) {
                    return fail("corrupt EXR chunk data");
                }
                detail::unfilter(decoded);
                rawData = decoded.data();
            }

            for (int l = 0; l < linesInChunk; ++l) {
                const int row = r0 + l; // top-down, matching STMapImage storage
                rowWritten[static_cast<size_t>(row)] = true;
                const uint8_t* line = rawData + static_cast<size_t>(l) * lineBytes;
                for (int comp = 0; comp < 2; ++comp) {
                    const detail::ExrChannel& ch = channels[static_cast<size_t>(comp == 0 ? rIndex : gIndex)];
                    const uint8_t* chData = line + ch.lineOffset;
                    float* dst = out->uv.data() + (static_cast<size_t>(row) * width) * 2 + comp;
                    if (ch.pixelType == 1) {
                        for (int x = 0; x < width; ++x) {
                            uint16_t h;
                            std::memcpy(&h, chData + static_cast<size_t>(x) * 2, 2);
                            dst[static_cast<size_t>(x) * 2] = detail::halfToFloat(h);
                        }
                    } else {
                        for (int x = 0; x < width; ++x) {
                            float v;
                            std::memcpy(&v, chData + static_cast<size_t>(x) * 4, 4);
                            dst[static_cast<size_t>(x) * 2] = v;
                        }
                    }
                }
            }
        }

        for (int row = 0; row < height; ++row) {
            if (!rowWritten[static_cast<size_t>(row)]) {
                return fail("EXR scanline data incomplete");
            }
        }
        return true;
    } catch (const std::bad_alloc&) {
        return fail("out of memory loading STMap");
    }
}

// ---------------------------------------------------------------------------
// Packed side-by-side STMap split (Canon VR-style authoring): one EXR whose
// left half is the left eye's map and right half the right eye's. Each half's
// U values can follow either convention — per-eye (spanning [0,1] against a
// single eye's frame) or packed-frame (the left source half is u∈[0,0.5], the
// right u∈[0.5,1]) — and the two are unambiguous in real maps: a fisheye→
// equirect per-eye map spans nearly the full U range, a packed-frame half
// sits inside half of it. So the split classifies each half from its VALID
// texels and rescales packed-frame U to per-eye ((u - offset) * 2). Detection
// is per half, which also absorbs maps that bake in the Canon eye swap (a
// destination half sampling the OTHER source half just gets the other
// offset). V is never touched — the packing is horizontal.
// ---------------------------------------------------------------------------

enum class PackedHalfCoords {
    PerEye,           // U already spans a single eye's frame — plain crop
    PackedLeftHalf,   // U referenced the packed frame's left half — rescaled
    PackedRightHalf,  // U referenced the packed frame's right half — rescaled
    NoValidTexels,    // nothing to classify — copied verbatim
};

// Classification thresholds: the MASS of a half's valid U values confined
// below kPackedDetectHigh (or above kPackedDetectLow) means packed-frame
// coords; real per-eye maps put heavy mass on both sides. Classification
// tolerates kPackedDetectMaxSpill of the valid texels on the wrong side —
// real maps pad unused regions with (0,0) filler (seen on the Canon EOS R5C
// RF5.2mm map, ~1% of a half), and a handful of zeros must not defeat the
// detection the way absolute min/max bounds did (2026-08-30 blob bug).
constexpr float kPackedDetectLow = 0.45f;
constexpr float kPackedDetectHigh = 0.55f;
constexpr float kPackedDetectMaxSpill = 0.05f;

inline const char* packedHalfCoordsName(PackedHalfCoords c)
{
    switch (c) {
        case PackedHalfCoords::PackedLeftHalf: return "packed-frame coords (left half, U rescaled)";
        case PackedHalfCoords::PackedRightHalf: return "packed-frame coords (right half, U rescaled)";
        case PackedHalfCoords::NoValidTexels: return "no valid texels (copied verbatim)";
        case PackedHalfCoords::PerEye:
        default: return "per-eye coords (copied)";
    }
}

namespace detail {

// Copy columns [x0, x0+outWidth) of `packed` into `out`, classifying the
// half's U convention and rescaling packed-frame U to per-eye. Invalid texels
// (NaN / outside [0,1]) are excluded from classification; the rescale keeps
// them invalid (NaN stays NaN, out-of-range values move further out).
inline PackedHalfCoords extractPackedHalf(const STMapImage& packed, int x0, STMapImage* out)
{
    const int outWidth = packed.width / 2;
    out->width = outWidth;
    out->height = packed.height;
    out->uv.assign(static_cast<size_t>(outWidth) * packed.height * 2, 0.0f);

    long long validCount = 0, belowLow = 0, aboveHigh = 0;
    for (int row = 0; row < packed.height; ++row) {
        const float* srcRow = packed.uv.data() +
                              (static_cast<size_t>(row) * packed.width + x0) * 2;
        for (int x = 0; x < outWidth; ++x) {
            const float u = srcRow[static_cast<size_t>(x) * 2];
            const float v = srcRow[static_cast<size_t>(x) * 2 + 1];
            if (u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f) {
                ++validCount;
                if (u < kPackedDetectLow) ++belowLow;
                if (u > kPackedDetectHigh) ++aboveHigh;
            }
        }
    }

    PackedHalfCoords coords = PackedHalfCoords::PerEye;
    float offset = 0.0f;
    if (validCount == 0) {
        coords = PackedHalfCoords::NoValidTexels;
    } else {
        const long long spillCap =
            static_cast<long long>(kPackedDetectMaxSpill * static_cast<double>(validCount));
        if (aboveHigh <= spillCap) {
            coords = PackedHalfCoords::PackedLeftHalf;
            offset = 0.0f;
        } else if (belowLow <= spillCap) {
            coords = PackedHalfCoords::PackedRightHalf;
            offset = 0.5f;
        }
    }

    const bool rescale = (coords == PackedHalfCoords::PackedLeftHalf ||
                          coords == PackedHalfCoords::PackedRightHalf);
    for (int row = 0; row < packed.height; ++row) {
        const float* srcRow = packed.uv.data() +
                              (static_cast<size_t>(row) * packed.width + x0) * 2;
        float* dstRow = out->uv.data() + static_cast<size_t>(row) * outWidth * 2;
        for (int x = 0; x < outWidth; ++x) {
            const float u = srcRow[static_cast<size_t>(x) * 2];
            dstRow[static_cast<size_t>(x) * 2] = rescale ? (u - offset) * 2.0f : u;
            dstRow[static_cast<size_t>(x) * 2 + 1] = srcRow[static_cast<size_t>(x) * 2 + 1];
        }
    }
    return coords;
}

} // namespace detail

// Split a side-by-side packed STMap into per-eye maps (left half → left eye).
// Returns false with *error on an unsplittable map; *leftCoords/*rightCoords
// report what each half's U convention was detected as (for the log — the
// caller should surface the decision).
inline bool splitPackedSTMap(const STMapImage& packed,
                             STMapImage* leftEye, STMapImage* rightEye,
                             PackedHalfCoords* leftCoords, PackedHalfCoords* rightCoords,
                             std::string* error)
{
    if (packed.width < 2 || (packed.width % 2) != 0 || packed.height < 1 ||
        packed.uv.size() != static_cast<size_t>(packed.width) * packed.height * 2) {
        if (error) {
            *error = "packed side-by-side STMap needs an even width (got " +
                     std::to_string(packed.width) + "x" + std::to_string(packed.height) + ")";
        }
        return false;
    }
    *leftCoords = detail::extractPackedHalf(packed, 0, leftEye);
    *rightCoords = detail::extractPackedHalf(packed, packed.width / 2, rightEye);
    return true;
}

namespace detail {

// Bilinear source fetch with integer clamps on every index, so even a
// nonsense sample position (the NaN worst case) can never read out of bounds.
inline void sampleBilinearClamped(const float* src, int srcWidth, int srcHeight, int srcRowFloats,
                                  float sx, float sy, float outPx[4])
{
    int x0 = static_cast<int>(std::floor(sx));
    int y0 = static_cast<int>(std::floor(sy));
    x0 = std::min(std::max(x0, 0), srcWidth - 1);
    y0 = std::min(std::max(y0, 0), srcHeight - 1);
    const int x1 = std::min(x0 + 1, srcWidth - 1);
    const int y1 = std::min(y0 + 1, srcHeight - 1);
    const float fx = std::min(std::max(sx - static_cast<float>(x0), 0.0f), 1.0f);
    const float fy = std::min(std::max(sy - static_cast<float>(y0), 0.0f), 1.0f);
    const float* p00 = src + static_cast<size_t>(y0) * srcRowFloats + static_cast<size_t>(x0) * 4;
    const float* p10 = src + static_cast<size_t>(y0) * srcRowFloats + static_cast<size_t>(x1) * 4;
    const float* p01 = src + static_cast<size_t>(y1) * srcRowFloats + static_cast<size_t>(x0) * 4;
    const float* p11 = src + static_cast<size_t>(y1) * srcRowFloats + static_cast<size_t>(x1) * 4;
    for (int c = 0; c < 4; ++c) {
        const float a = p00[c] + (p10[c] - p00[c]) * fx;
        const float b = p01[c] + (p11[c] - p01[c]) * fx;
        outPx[c] = a + (b - a) * fy;
    }
}

} // namespace detail

// STMap-driven warp of float RGBA (see the header comment for conventions).
// src is bottom-up with srcRowFloats stride; mapUV is interleaved (u,v) with
// row 0 = top; dst is tightly packed bottom-up outWidth*outHeight*4 floats,
// where (outWidth, outHeight) = ndi_stream::outputDims(mapWidth, mapHeight,
// divisor).
inline void warpRGBABox(const float* src, int srcWidth, int srcHeight, int srcRowFloats,
                        const float* mapUV, int mapWidth, int mapHeight,
                        int divisor, float* dst, int outWidth, int outHeight)
{
    const float invCount = 1.0f / static_cast<float>(divisor * divisor);
    for (int y = 0; y < outHeight; ++y) {
        for (int x = 0; x < outWidth; ++x) {
            float sum[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            for (int sy = 0; sy < divisor; ++sy) {
                const int dstY = std::min(y * divisor + sy, mapHeight - 1); // bottom-up full-res row
                const int mapRow = mapHeight - 1 - dstY;                    // map stored top-down
                for (int sx = 0; sx < divisor; ++sx) {
                    const int dstX = std::min(x * divisor + sx, mapWidth - 1);
                    const float* mt = mapUV + (static_cast<size_t>(mapRow) * mapWidth + dstX) * 2;
                    const float u = mt[0];
                    const float v = mt[1];
                    if (u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f) {
                        float px[4];
                        detail::sampleBilinearClamped(src, srcWidth, srcHeight, srcRowFloats,
                                                      u * static_cast<float>(srcWidth) - 0.5f,
                                                      v * static_cast<float>(srcHeight) - 0.5f, px);
                        sum[0] += px[0];
                        sum[1] += px[1];
                        sum[2] += px[2];
                        sum[3] += px[3];
                    } // out-of-range/NaN taps are outside the lens circle: black
                }
            }
            float* o = dst + (static_cast<size_t>(y) * outWidth + x) * 4;
            o[0] = sum[0] * invCount;
            o[1] = sum[1] * invCount;
            o[2] = sum[2] * invCount;
            o[3] = sum[3] * invCount;
        }
    }
}

} // namespace ndi_stmap

#endif
