// Tests for the STMap projection seam (src/STMap.h): the minimal EXR reader
// that loads Fusion-authored STMaps and the CPU reference warp used when
// frames arrive on the CPU (and as the Metal kernels' reference).
//
// The reader is validated two ways:
//  - against REAL third-party EXRs (tests/fixtures/*.exr, written by ffmpeg's
//    OpenEXR encoder) so the reader can't merely agree with our own writer's
//    misunderstanding of the format;
//  - against in-test-built EXRs for the layouts ffmpeg doesn't produce
//    (extra channels, DECREASING_Y, the raw-fallback chunk) and for the
//    hostile inputs that must fail soft (never crash — issue #7).
// Build & run: make test
//
// Fixture provenance: 32x16 identity STMap (R = (x+0.5)/32, G = (ybottom+0.5)/16,
// B = 0.25), one file per compression (none/RLE/ZIP1/ZIP16) plus a half-float
// ZIP16 variant. All values chosen exactly representable in half and float.

#include "STMap.h"
#include "StreamResolution.h"

#include <zlib.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
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

static void expectFloat(float actual, float expected, const char* name)
{
    // Values in these tests are exactly representable; exact compare intended.
    if (actual != expected) {
        ++failures;
        std::fprintf(stderr, "FAIL %s\n  expected: %.9g\n  actual:   %.9g\n", name, expected, actual);
    } else {
        std::printf("ok   %s\n", name);
    }
}

// --------------------------------------------------------------------------
// Minimal EXR writer (test fixture author). Single-part scanline only.
// --------------------------------------------------------------------------

namespace exrw {

enum { kCompNone = 0, kCompRLE = 1, kCompZIPS = 2, kCompZIP = 3 };
enum { kTypeUInt = 0, kTypeHalf = 1, kTypeFloat = 2 };

struct Channel {
    std::string name;
    int pixelType = kTypeFloat;
    int xSampling = 1;
    int ySampling = 1;
};

static void put8(std::vector<uint8_t>& b, uint8_t v) { b.push_back(v); }
static void put32(std::vector<uint8_t>& b, int32_t v)
{
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>((static_cast<uint32_t>(v) >> (i * 8)) & 0xFF));
}
static void put64(std::vector<uint8_t>& b, uint64_t v)
{
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
}
static void putStr(std::vector<uint8_t>& b, const std::string& s)
{
    for (char c : s) b.push_back(static_cast<uint8_t>(c));
    b.push_back(0);
}
static void putBytes(std::vector<uint8_t>& b, const void* p, size_t n)
{
    const uint8_t* c = static_cast<const uint8_t*>(p);
    b.insert(b.end(), c, c + n);
}
static void putAttr(std::vector<uint8_t>& b, const std::string& name, const std::string& type,
                    const std::vector<uint8_t>& value)
{
    putStr(b, name);
    putStr(b, type);
    put32(b, static_cast<int32_t>(value.size()));
    putBytes(b, value.data(), value.size());
}

static uint16_t floatToHalf(float f)
{
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    const uint32_t sign = (bits >> 16) & 0x8000u;
    const int32_t exp = static_cast<int32_t>((bits >> 23) & 0xFF) - 127 + 15;
    const uint32_t man = bits & 0x7FFFFFu;
    if (exp <= 0) return static_cast<uint16_t>(sign);               // test values avoid subnormals
    if (exp >= 31) return static_cast<uint16_t>(sign | 0x7C00u);
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exp) << 10) | (man >> 13)); // test values exact
}

// OpenEXR's pre-deflate filter: deinterleave bytes into two halves, then
// delta-encode against the previous RAW byte (+128 mod 256).
static std::vector<uint8_t> filter(const std::vector<uint8_t>& raw)
{
    const size_t n = raw.size();
    std::vector<uint8_t> t(n);
    const size_t half = (n + 1) / 2;
    size_t i1 = 0, i2 = half;
    for (size_t i = 0; i < n; ++i) {
        if ((i & 1) == 0) t[i1++] = raw[i];
        else t[i2++] = raw[i];
    }
    std::vector<uint8_t> out(n);
    if (n) out[0] = t[0];
    int prev = n ? t[0] : 0;
    for (size_t i = 1; i < n; ++i) {
        out[i] = static_cast<uint8_t>((static_cast<int>(t[i]) - prev + 384) & 0xFF);
        prev = t[i];
    }
    return out;
}

static std::vector<uint8_t> zlibDeflate(const std::vector<uint8_t>& in)
{
    uLongf outLen = compressBound(static_cast<uLong>(in.size()));
    std::vector<uint8_t> out(outLen);
    if (compress2(out.data(), &outLen, in.data(), static_cast<uLong>(in.size()), 9) != Z_OK) {
        std::fprintf(stderr, "test writer: compress2 failed\n");
        std::exit(1);
    }
    out.resize(outLen);
    return out;
}

// count>=3 same byte -> run (n-1, byte); else literal batch (-n, bytes).
static std::vector<uint8_t> rleEncode(const std::vector<uint8_t>& in)
{
    std::vector<uint8_t> out;
    size_t i = 0;
    while (i < in.size()) {
        size_t run = 1;
        while (i + run < in.size() && in[i + run] == in[i] && run < 128) ++run;
        if (run >= 3) {
            out.push_back(static_cast<uint8_t>(run - 1));
            out.push_back(in[i]);
            i += run;
        } else {
            size_t lit = 0;
            while (i + lit < in.size() && lit < 127) {
                size_t r = 1;
                while (i + lit + r < in.size() && in[i + lit + r] == in[i + lit] && r < 3) ++r;
                if (r >= 3) break;
                ++lit;
            }
            if (lit == 0) lit = 1;
            out.push_back(static_cast<uint8_t>(0x100 - lit)); // -lit as int8
            for (size_t k = 0; k < lit; ++k) out.push_back(in[i + k]);
            i += lit;
        }
    }
    return out;
}

struct BuildOptions {
    int compression = kCompNone;
    int lineOrder = 0;              // 0 = INCREASING_Y, 1 = DECREASING_Y
    bool chunksReversedInFile = false;
    int forceRawChunk = -1;         // chunk index stored raw/unfiltered (ZIP raw fallback)
    uint32_t versionWord = 2;       // patched for tiled/multipart/deep error tests
    int compressionByteOverride = -1; // e.g. PIZ to test the unsupported-compression error
};

// channels must be pre-sorted alphabetically (like real files); values[ch] is
// row-major with row 0 = TOP row (EXR storage order).
static std::vector<uint8_t> buildEXR(int width, int height,
                                     const std::vector<Channel>& channels,
                                     const std::vector<std::vector<float>>& values,
                                     const BuildOptions& opt = BuildOptions())
{
    std::vector<uint8_t> b;
    put8(b, 0x76); put8(b, 0x2f); put8(b, 0x31); put8(b, 0x01); // magic
    put32(b, static_cast<int32_t>(opt.versionWord));

    // channels attribute
    std::vector<uint8_t> chlist;
    for (const Channel& c : channels) {
        putStr(chlist, c.name);
        put32(chlist, c.pixelType);
        put8(chlist, 0); put8(chlist, 0); put8(chlist, 0); put8(chlist, 0); // pLinear + reserved
        put32(chlist, c.xSampling);
        put32(chlist, c.ySampling);
    }
    put8(chlist, 0); // end of list
    putAttr(b, "channels", "chlist", chlist);

    {
        std::vector<uint8_t> v;
        put8(v, static_cast<uint8_t>(opt.compressionByteOverride >= 0 ? opt.compressionByteOverride
                                                                      : opt.compression));
        putAttr(b, "compression", "compression", v);
    }
    {
        std::vector<uint8_t> v;
        put32(v, 0); put32(v, 0); put32(v, width - 1); put32(v, height - 1);
        putAttr(b, "dataWindow", "box2i", v);
        putAttr(b, "displayWindow", "box2i", v);
    }
    {
        std::vector<uint8_t> v;
        put8(v, static_cast<uint8_t>(opt.lineOrder));
        putAttr(b, "lineOrder", "lineOrder", v);
    }
    {
        std::vector<uint8_t> v;
        put32(v, 0x3f800000); // 1.0f
        putAttr(b, "pixelAspectRatio", "float", v);
        // an attribute type the reader has never heard of — must be skipped
        std::vector<uint8_t> junk = {1, 2, 3, 4, 5, 6, 7};
        putAttr(b, "fusionNodeName", "string", junk);
    }
    put8(b, 0); // end of header

    const int lpb = (opt.compression == kCompZIP) ? 16 : 1;
    const int chunkCount = (height + lpb - 1) / lpb;

    size_t bppLine = 0;
    for (const Channel& c : channels) {
        bppLine += static_cast<size_t>(width) * (c.pixelType == kTypeHalf ? 2 : 4);
    }

    // Encode every chunk first, then the offset table, then the chunks.
    std::vector<std::vector<uint8_t>> chunkData(chunkCount);
    std::vector<int> chunkY(chunkCount);
    for (int ci = 0; ci < chunkCount; ++ci) {
        const int y0 = ci * lpb;
        const int lines = std::min(lpb, height - y0);
        chunkY[ci] = y0;
        std::vector<uint8_t> raw;
        raw.reserve(static_cast<size_t>(lines) * bppLine);
        for (int l = 0; l < lines; ++l) {
            const int row = y0 + l;
            for (size_t ch = 0; ch < channels.size(); ++ch) {
                for (int x = 0; x < width; ++x) {
                    const float f = values[ch][static_cast<size_t>(row) * width + x];
                    if (channels[ch].pixelType == kTypeHalf) {
                        const uint16_t h = floatToHalf(f);
                        putBytes(raw, &h, 2);
                    } else {
                        putBytes(raw, &f, 4);
                    }
                }
            }
        }
        if (ci == opt.forceRawChunk || opt.compression == kCompNone) {
            chunkData[ci] = raw;
        } else if (opt.compression == kCompRLE) {
            chunkData[ci] = rleEncode(filter(raw));
        } else {
            chunkData[ci] = zlibDeflate(filter(raw));
        }
    }

    const size_t tableStart = b.size();
    const size_t chunksStart = tableStart + static_cast<size_t>(chunkCount) * 8;
    std::vector<uint64_t> offsets(chunkCount);
    {
        size_t off = chunksStart;
        for (int i = 0; i < chunkCount; ++i) {
            const int ci = opt.chunksReversedInFile ? (chunkCount - 1 - i) : i;
            offsets[ci] = off;
            off += 8 + chunkData[ci].size();
        }
    }
    for (int ci = 0; ci < chunkCount; ++ci) put64(b, offsets[ci]);
    for (int i = 0; i < chunkCount; ++i) {
        const int ci = opt.chunksReversedInFile ? (chunkCount - 1 - i) : i;
        put32(b, chunkY[ci]);
        put32(b, static_cast<int32_t>(chunkData[ci].size()));
        putBytes(b, chunkData[ci].data(), chunkData[ci].size());
    }
    return b;
}

} // namespace exrw

// --------------------------------------------------------------------------
// Helpers
// --------------------------------------------------------------------------

static std::string writeTemp(const char* name, const std::vector<uint8_t>& bytes)
{
    ::mkdir("build", 0755); // ignore EEXIST — make test creates it anyway
    const std::string path = std::string("build/") + name;
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        std::exit(1);
    }
    if (!bytes.empty()) std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    return path;
}

// The 32x16 identity-map pattern all fixtures carry (see fixture provenance).
static void checkIdentityFixture(const char* path, const char* label)
{
    ndi_stmap::STMapImage map;
    std::string err;
    const bool ok = ndi_stmap::loadSTMapEXR(path, &map, &err);
    char name[160];
    std::snprintf(name, sizeof(name), "%s loads", label);
    check(ok && map.width == 32 && map.height == 16, name);
    if (!ok) {
        std::fprintf(stderr, "  error: %s\n", err.c_str());
        return;
    }
    bool valuesOk = true;
    for (int row = 0; row < 16 && valuesOk; ++row) {      // row 0 = top
        const float v = (16 - 1 - row + 0.5f) / 16.0f;    // bottom-up v
        for (int x = 0; x < 32; ++x) {
            const float u = (x + 0.5f) / 32.0f;
            const size_t i = (static_cast<size_t>(row) * 32 + x) * 2;
            if (map.uv[i] != u || map.uv[i + 1] != v) {
                std::fprintf(stderr, "  mismatch at (%d,%d): got (%.9g,%.9g) want (%.9g,%.9g)\n",
                             x, row, map.uv[i], map.uv[i + 1], u, v);
                valuesOk = false;
                break;
            }
        }
    }
    std::snprintf(name, sizeof(name), "%s values match the identity pattern", label);
    check(valuesOk, name);
}

static void checkLoadFails(const std::vector<uint8_t>& bytes, const char* tmpName, const char* label)
{
    const std::string path = writeTemp(tmpName, bytes);
    ndi_stmap::STMapImage map;
    std::string err;
    const bool ok = ndi_stmap::loadSTMapEXR(path.c_str(), &map, &err);
    char name[160];
    std::snprintf(name, sizeof(name), "%s fails soft with an error message", label);
    check(!ok && !err.empty(), name);
}

// row 0 = TOP (matches STMapImage storage); u/v use the bottom-left convention.
static std::vector<float> identityMapUV(int w, int h)
{
    std::vector<float> uv(static_cast<size_t>(w) * h * 2);
    for (int row = 0; row < h; ++row) {
        const float v = (h - 1 - row + 0.5f) / static_cast<float>(h);
        for (int x = 0; x < w; ++x) {
            uv[(static_cast<size_t>(row) * w + x) * 2 + 0] = (x + 0.5f) / static_cast<float>(w);
            uv[(static_cast<size_t>(row) * w + x) * 2 + 1] = v;
        }
    }
    return uv;
}

static std::vector<float> patternFrame(int w, int h, int rowFloats)
{
    std::vector<float> f(static_cast<size_t>(h) * rowFloats, 999.0f); // poison padding
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float* px = f.data() + static_cast<size_t>(y) * rowFloats + static_cast<size_t>(x) * 4;
            px[0] = static_cast<float>((x * 13 + y * 7) % 17) / 16.0f;
            px[1] = static_cast<float>((x * 5 + y * 11) % 13) / 16.0f;
            px[2] = static_cast<float>((x + y) % 9) / 8.0f;
            px[3] = static_cast<float>((x * 3 + y) % 5) / 4.0f;
        }
    }
    return f;
}

int main()
{
    // ---- Reader vs REAL third-party files (ffmpeg's OpenEXR encoder) ----
    checkIdentityFixture("tests/fixtures/identity_none_f32.exr", "fixture float/uncompressed");
    checkIdentityFixture("tests/fixtures/identity_rle_f32.exr", "fixture float/RLE");
    checkIdentityFixture("tests/fixtures/identity_zip1_f32.exr", "fixture float/ZIPS");
    checkIdentityFixture("tests/fixtures/identity_zip16_f32.exr", "fixture float/ZIP16");
    checkIdentityFixture("tests/fixtures/identity_zip16_f16.exr", "fixture half/ZIP16");

    // ---- Reader vs in-test-built files ----
    const int W = 32, H = 20; // 20 rows: a full 16-line ZIP chunk plus a short one
    std::vector<float> uPlane(static_cast<size_t>(W) * H), vPlane(uPlane.size());
    std::vector<float> junkPlane(uPlane.size(), 0.7f), alphaPlane(uPlane.size(), 0.9f);
    for (int row = 0; row < H; ++row) {
        for (int x = 0; x < W; ++x) {
            uPlane[static_cast<size_t>(row) * W + x] = (x + 0.5f) / W;
            vPlane[static_cast<size_t>(row) * W + x] = (H - 1 - row + 0.5f) / H;
        }
    }
    auto checkBuilt = [&](const char* path, const char* label, bool half = false) {
        ndi_stmap::STMapImage map;
        std::string err;
        const bool ok = ndi_stmap::loadSTMapEXR(path, &map, &err);
        char name[160];
        std::snprintf(name, sizeof(name), "%s loads", label);
        check(ok && map.width == W && map.height == H, name);
        if (!ok) {
            std::fprintf(stderr, "  error: %s\n", err.c_str());
            return;
        }
        // Half fixtures round-trip through 16-bit: quantize the expectation
        // through the same conversion the writer applied.
        const auto expected = [&](float v) {
            return half ? ndi_stmap::detail::halfToFloat(exrw::floatToHalf(v)) : v;
        };
        bool valuesOk = true;
        for (size_t i = 0; i < uPlane.size() && valuesOk; ++i) {
            valuesOk = (map.uv[i * 2] == expected(uPlane[i])) &&
                       (map.uv[i * 2 + 1] == expected(vPlane[i]));
        }
        std::snprintf(name, sizeof(name), "%s values land in the right rows/channels", label);
        check(valuesOk, name);
    };

    {
        // Four channels A,B,G,R (alphabetical): per-line channel offsets must
        // skip A and B correctly.
        std::vector<exrw::Channel> chans = {{"A"}, {"B"}, {"G"}, {"R"}};
        std::vector<std::vector<float>> vals = {alphaPlane, junkPlane, vPlane, uPlane};
        exrw::BuildOptions opt;
        opt.compression = exrw::kCompZIP;
        checkBuilt(writeTemp("abgr_zip.exr", exrw::buildEXR(W, H, chans, vals, opt)).c_str(),
                   "built ABGR/ZIP16");
    }
    {
        // DECREASING_Y with the chunks physically reversed in the file: the
        // per-chunk y field must place every row, whatever the file order.
        std::vector<exrw::Channel> chans = {{"G"}, {"R"}};
        std::vector<std::vector<float>> vals = {vPlane, uPlane};
        exrw::BuildOptions opt;
        opt.compression = exrw::kCompZIPS;
        opt.lineOrder = 1;
        opt.chunksReversedInFile = true;
        checkBuilt(writeTemp("decy_zips.exr", exrw::buildEXR(W, H, chans, vals, opt)).c_str(),
                   "built DECREASING_Y/ZIPS reversed chunks");
    }
    {
        // ZIP raw fallback: OpenEXR stores a chunk RAW (and unfiltered) when
        // deflate wouldn't shrink it; the reader keys on dataSize == raw size.
        std::vector<exrw::Channel> chans = {{"G"}, {"R"}};
        std::vector<std::vector<float>> vals = {vPlane, uPlane};
        exrw::BuildOptions opt;
        opt.compression = exrw::kCompZIPS;
        opt.forceRawChunk = 3;
        checkBuilt(writeTemp("rawchunk_zips.exr", exrw::buildEXR(W, H, chans, vals, opt)).c_str(),
                   "built ZIPS with one raw-fallback chunk");
    }
    {
        // Half channels through the in-test writer (fixture covers ffmpeg's).
        std::vector<exrw::Channel> chans = {{"G", exrw::kTypeHalf}, {"R", exrw::kTypeHalf}};
        std::vector<std::vector<float>> vals = {vPlane, uPlane};
        exrw::BuildOptions opt;
        opt.compression = exrw::kCompRLE;
        checkBuilt(writeTemp("half_rle.exr", exrw::buildEXR(W, H, chans, vals, opt)).c_str(),
                   "built half/RLE", /*half=*/true);
    }

    // ---- Hostile / unsupported inputs: fail soft, never crash ----
    {
        std::vector<exrw::Channel> chans = {{"G"}, {"R"}};
        std::vector<std::vector<float>> vals = {vPlane, uPlane};

        std::vector<uint8_t> good = exrw::buildEXR(W, H, chans, vals);

        std::vector<uint8_t> badMagic = good;
        badMagic[0] = 0x50;
        checkLoadFails(badMagic, "bad_magic.exr", "wrong magic");

        exrw::BuildOptions tiled; tiled.versionWord = 2 | 0x200;
        checkLoadFails(exrw::buildEXR(W, H, chans, vals, tiled), "tiled.exr", "tiled flag");
        exrw::BuildOptions multi; multi.versionWord = 2 | 0x1000;
        checkLoadFails(exrw::buildEXR(W, H, chans, vals, multi), "multipart.exr", "multipart flag");
        exrw::BuildOptions deep; deep.versionWord = 2 | 0x800;
        checkLoadFails(exrw::buildEXR(W, H, chans, vals, deep), "deep.exr", "deep flag");

        exrw::BuildOptions piz; piz.compressionByteOverride = 4;
        checkLoadFails(exrw::buildEXR(W, H, chans, vals, piz), "piz.exr", "PIZ compression");

        std::vector<exrw::Channel> noG = {{"B"}, {"R"}};
        std::vector<std::vector<float>> noGVals = {junkPlane, uPlane};
        checkLoadFails(exrw::buildEXR(W, H, noG, noGVals), "no_g.exr", "missing G channel");

        std::vector<exrw::Channel> uintR = {{"G"}, {"R", exrw::kTypeUInt}};
        checkLoadFails(exrw::buildEXR(W, H, uintR, vals), "uint_r.exr", "UINT R channel");

        std::vector<exrw::Channel> subsampled = {{"G", exrw::kTypeFloat, 1, 2}, {"R"}};
        checkLoadFails(exrw::buildEXR(W, H, subsampled, vals), "subsampled.exr", "subsampled channel");

        std::vector<uint8_t> truncHeader(good.begin(), good.begin() + 40);
        checkLoadFails(truncHeader, "trunc_header.exr", "truncated header");

        std::vector<uint8_t> truncChunks(good.begin(), good.begin() + good.size() * 3 / 5);
        checkLoadFails(truncChunks, "trunc_chunks.exr", "truncated pixel data");

        checkLoadFails({}, "empty.exr", "empty file");

        // dataWindow claiming absurd dimensions must be rejected before any
        // allocation, not crash trying.
        std::vector<uint8_t> huge = exrw::buildEXR(W, H, chans, vals);
        // patch dataWindow xMax (found by scanning for the attr name)
        const char* needle = "dataWindow";
        for (size_t i = 0; i + 32 < huge.size(); ++i) {
            if (std::memcmp(huge.data() + i, needle, 10) == 0) {
                // name\0 box2i\0 size(4) then xMin yMin xMax yMax
                size_t base = i + 11 + 6 + 4;
                int32_t big = 2000000000;
                std::memcpy(huge.data() + base + 8, &big, 4);
                std::memcpy(huge.data() + base + 12, &big, 4);
                break;
            }
        }
        checkLoadFails(huge, "huge_dims.exr", "absurd dataWindow");

        ndi_stmap::STMapImage map;
        std::string err;
        check(!ndi_stmap::loadSTMapEXR("build/does_not_exist_stmap.exr", &map, &err) && !err.empty(),
              "missing file fails soft with an error message");
    }

    // ---- warpRGBABox: the CPU reference warp ----
    {
        // Identity map at source dims (powers of two -> exact float positions)
        // must reproduce the plain box downscale bit-for-bit: same taps, same
        // accumulation order, bilinear weights degenerate to exact fetches.
        const int srcW = 8, srcH = 4;
        std::vector<float> src = patternFrame(srcW, srcH, srcW * 4);
        std::vector<float> ident = identityMapUV(srcW, srcH);
        for (int divisor : {1, 2}) {
            int outW = 0, outH = 0;
            ndi_stream::outputDims(srcW, srcH, divisor, &outW, &outH);
            std::vector<float> expected(static_cast<size_t>(outW) * outH * 4, -1.0f);
            ndi_stream::downscaleRGBABox(src.data(), srcW, srcH, srcW * 4, divisor,
                                         expected.data(), outW, outH);
            std::vector<float> actual(expected.size(), -2.0f);
            ndi_stmap::warpRGBABox(src.data(), srcW, srcH, srcW * 4,
                                   ident.data(), srcW, srcH, divisor,
                                   actual.data(), outW, outH);
            char name[128];
            std::snprintf(name, sizeof(name), "identity map == box downscale (divisor %d)", divisor);
            check(std::memcmp(actual.data(), expected.data(), expected.size() * 4) == 0, name);
        }
    }
    {
        // Constant map: every output pixel samples the exact center of source
        // pixel (2,1) of a 4x2 frame.
        const int srcW = 4, srcH = 2;
        std::vector<float> src = patternFrame(srcW, srcH, srcW * 4);
        const float* target = src.data() + (1 * srcW + 2) * 4;
        std::vector<float> uv(3 * 3 * 2);
        for (size_t i = 0; i < 9; ++i) {
            uv[i * 2 + 0] = (2 + 0.5f) / srcW;
            uv[i * 2 + 1] = (1 + 0.5f) / srcH;
        }
        std::vector<float> out(3 * 3 * 4, -1.0f);
        ndi_stmap::warpRGBABox(src.data(), srcW, srcH, srcW * 4, uv.data(), 3, 3, 1,
                               out.data(), 3, 3);
        bool ok = true;
        for (int i = 0; i < 9; ++i) {
            for (int c = 0; c < 4; ++c) ok = ok && (out[i * 4 + c] == target[c]);
        }
        check(ok, "constant map replicates the mapped source pixel everywhere");
    }
    {
        // Bilinear midpoint: u halfway between the centers of pixels 0 and 1
        // averages them; alpha interpolates too.
        const int srcW = 4, srcH = 1;
        std::vector<float> src(srcW * 4, 0.0f);
        src[0] = 0.25f; src[3] = 1.0f;   // pixel 0: R=0.25, A=1
        src[4] = 0.75f; src[7] = 0.5f;   // pixel 1: R=0.75, A=0.5
        std::vector<float> uv = {0.25f, 0.5f}; // 1x1 map; sx = 0.25*4-0.5 = 0.5
        std::vector<float> out(4, -1.0f);
        ndi_stmap::warpRGBABox(src.data(), srcW, srcH, srcW * 4, uv.data(), 1, 1, 1,
                               out.data(), 1, 1);
        expectFloat(out[0], 0.5f, "bilinear midpoint averages R");
        expectFloat(out[3], 0.75f, "bilinear midpoint averages A");
    }
    {
        // Out-of-range and NaN map values contribute transparent black.
        const int srcW = 2, srcH = 2;
        std::vector<float> src(srcW * srcH * 4, 0.8f);
        std::vector<float> uv = {1.5f, 0.5f, 0.5f, -0.25f,
                                 std::nanf(""), 0.5f, 0.5f, std::nanf("")};
        std::vector<float> out(4 * 4, -1.0f);
        ndi_stmap::warpRGBABox(src.data(), srcW, srcH, srcW * 4, uv.data(), 2, 2, 1,
                               out.data(), 2, 2);
        bool ok = true;
        for (int i = 0; i < 16; ++i) ok = ok && (out[i] == 0.0f);
        check(ok, "out-of-range and NaN map values produce black");
    }
    {
        // v orientation: the map's TOP row (row 0) drives the OUTPUT's top row
        // (bottom-up row outH-1), and v near 1 samples the source's TOP row.
        const int srcW = 1, srcH = 2;
        std::vector<float> src(srcH * 4, 0.0f);
        src[0] = 0.1f;              // source row 0 = bottom
        src[4] = 0.9f;              // source row 1 = top
        std::vector<float> uv = {0.5f, 0.75f,  // map row 0 (top): v=0.75 -> source top
                                 0.5f, 0.25f}; // map row 1 (bottom): v=0.25 -> source bottom
        std::vector<float> out(2 * 4, -1.0f);
        ndi_stmap::warpRGBABox(src.data(), srcW, srcH, srcW * 4, uv.data(), 1, 2, 1,
                               out.data(), 1, 2);
        expectFloat(out[0], 0.1f, "output bottom row uses map bottom row (v=0.25 -> source bottom)");
        expectFloat(out[4], 0.9f, "output top row uses map top row (v=0.75 -> source top)");
    }
    {
        // Composition: warp at divisor 2 must equal warp at map resolution
        // followed by the box downscale — the definition the Metal kernel
        // also implements.
        const int srcW = 8, srcH = 8, mapW = 6, mapH = 6;
        std::vector<float> src = patternFrame(srcW, srcH, srcW * 4);
        std::vector<float> uv(static_cast<size_t>(mapW) * mapH * 2);
        for (int row = 0; row < mapH; ++row) {
            for (int x = 0; x < mapW; ++x) {
                const float xn = (x + 0.5f) / mapW;
                const float yn = (mapH - 1 - row + 0.5f) / mapH;
                uv[(static_cast<size_t>(row) * mapW + x) * 2 + 0] = xn * xn;        // nonlinear
                uv[(static_cast<size_t>(row) * mapW + x) * 2 + 1] = 0.1f + 0.8f * yn;
            }
        }
        int outW = 0, outH = 0;
        ndi_stream::outputDims(mapW, mapH, 2, &outW, &outH);
        std::vector<float> full(static_cast<size_t>(mapW) * mapH * 4, -1.0f);
        ndi_stmap::warpRGBABox(src.data(), srcW, srcH, srcW * 4, uv.data(), mapW, mapH, 1,
                               full.data(), mapW, mapH);
        std::vector<float> expected(static_cast<size_t>(outW) * outH * 4, -1.0f);
        ndi_stream::downscaleRGBABox(full.data(), mapW, mapH, mapW * 4, 2,
                                     expected.data(), outW, outH);
        std::vector<float> actual(expected.size(), -2.0f);
        ndi_stmap::warpRGBABox(src.data(), srcW, srcH, srcW * 4, uv.data(), mapW, mapH, 2,
                               actual.data(), outW, outH);
        check(std::memcmp(actual.data(), expected.data(), expected.size() * 4) == 0,
              "warp divisor 2 == full-res warp then box downscale");
    }
    {
        // Row stride honored: padded source rows give the same result as tight.
        const int srcW = 8, srcH = 4;
        const int paddedRowFloats = (srcW + 3) * 4;
        std::vector<float> tight = patternFrame(srcW, srcH, srcW * 4);
        std::vector<float> padded = patternFrame(srcW, srcH, paddedRowFloats);
        std::vector<float> uv(2 * 2 * 2);
        uv[0] = 0.3f; uv[1] = 0.6f; uv[2] = 0.9f; uv[3] = 0.2f;
        uv[4] = 0.1f; uv[5] = 0.8f; uv[6] = 0.5f; uv[7] = 0.5f;
        std::vector<float> outTight(2 * 2 * 4, -1.0f), outPadded(2 * 2 * 4, -2.0f);
        ndi_stmap::warpRGBABox(tight.data(), srcW, srcH, srcW * 4, uv.data(), 2, 2, 1,
                               outTight.data(), 2, 2);
        ndi_stmap::warpRGBABox(padded.data(), srcW, srcH, paddedRowFloats, uv.data(), 2, 2, 1,
                               outPadded.data(), 2, 2);
        check(std::memcmp(outTight.data(), outPadded.data(), outTight.size() * 4) == 0,
              "source row stride honored (padding never leaks in)");
    }

    // ---- splitPackedSTMap: one side-by-side packed map -> per-eye maps ----
    {
        using ndi_stmap::PackedHalfCoords;
        const int W = 8, H = 2, halfW = 4;

        // Build a packed map: per-column u values, v = column-independent
        // per-row values (must survive the split untouched).
        auto makePacked = [&](const float uCols[8]) {
            std::vector<float> uv(static_cast<size_t>(W) * H * 2);
            for (int row = 0; row < H; ++row) {
                for (int x = 0; x < W; ++x) {
                    uv[(static_cast<size_t>(row) * W + x) * 2 + 0] = uCols[x];
                    uv[(static_cast<size_t>(row) * W + x) * 2 + 1] = row == 0 ? 0.75f : 0.25f;
                }
            }
            return uv;
        };
        auto uAt = [&](const ndi_stmap::STMapImage& m, int x, int row) {
            return m.uv[(static_cast<size_t>(row) * m.width + x) * 2 + 0];
        };
        auto vAt = [&](const ndi_stmap::STMapImage& m, int x, int row) {
            return m.uv[(static_cast<size_t>(row) * m.width + x) * 2 + 1];
        };

        // Per-eye coords: both halves span nearly the full [0,1] u range.
        {
            const float uCols[8] = {0.125f, 0.375f, 0.625f, 0.875f,   // left half
                                    0.875f, 0.625f, 0.375f, 0.125f};  // right half
            ndi_stmap::STMapImage packed;
            packed.width = W; packed.height = H; packed.uv = makePacked(uCols);
            ndi_stmap::STMapImage left, right;
            PackedHalfCoords cl, cr;
            std::string err;
            bool ok = ndi_stmap::splitPackedSTMap(packed, &left, &right, &cl, &cr, &err);
            check(ok && left.width == halfW && right.width == halfW &&
                      left.height == H && right.height == H,
                  "per-eye-coords split: halves have half width, same height");
            check(cl == PackedHalfCoords::PerEye && cr == PackedHalfCoords::PerEye,
                  "per-eye-coords split: both halves detected as per-eye");
            expectFloat(uAt(left, 1, 0), 0.375f, "per-eye-coords split copies left u verbatim");
            expectFloat(uAt(right, 3, 1), 0.125f, "per-eye-coords split copies right u verbatim");
            expectFloat(vAt(left, 2, 0), 0.75f, "split leaves v untouched (row 0)");
            expectFloat(vAt(right, 0, 1), 0.25f, "split leaves v untouched (row 1)");
        }

        // Packed-frame coords: left half samples u in [0,0.5], right in
        // [0.5,1]; the split rescales each half to per-eye [0,1].
        {
            const float uCols[8] = {0.0625f, 0.1875f, 0.3125f, 0.4375f,
                                    0.5625f, 0.6875f, 0.8125f, 0.9375f};
            ndi_stmap::STMapImage packed;
            packed.width = W; packed.height = H; packed.uv = makePacked(uCols);
            ndi_stmap::STMapImage left, right;
            PackedHalfCoords cl, cr;
            std::string err;
            bool ok = ndi_stmap::splitPackedSTMap(packed, &left, &right, &cl, &cr, &err);
            check(ok && cl == PackedHalfCoords::PackedLeftHalf &&
                      cr == PackedHalfCoords::PackedRightHalf,
                  "packed-frame coords detected on both halves");
            expectFloat(uAt(left, 0, 0), 0.125f, "left half u rescaled x2");
            expectFloat(uAt(left, 3, 0), 0.875f, "left half u rescaled x2 (last column)");
            expectFloat(uAt(right, 0, 0), 0.125f, "right half u rescaled (-0.5)*2");
            expectFloat(uAt(right, 3, 1), 0.875f, "right half u rescaled (-0.5)*2 (last column)");
            expectFloat(vAt(left, 1, 0), 0.75f, "packed-frame split leaves v untouched");
        }

        // Canon-style swap baked into the map: the left DESTINATION half
        // samples the RIGHT source half. Detection is per half, so the
        // rescale offset follows where the values sit.
        {
            const float uCols[8] = {0.5625f, 0.6875f, 0.8125f, 0.9375f,
                                    0.0625f, 0.1875f, 0.3125f, 0.4375f};
            ndi_stmap::STMapImage packed;
            packed.width = W; packed.height = H; packed.uv = makePacked(uCols);
            ndi_stmap::STMapImage left, right;
            PackedHalfCoords cl, cr;
            std::string err;
            bool ok = ndi_stmap::splitPackedSTMap(packed, &left, &right, &cl, &cr, &err);
            check(ok && cl == PackedHalfCoords::PackedRightHalf &&
                      cr == PackedHalfCoords::PackedLeftHalf,
                  "swapped packed-frame halves each detect their own offset");
            expectFloat(uAt(left, 0, 0), 0.125f, "swapped left half rescales from [0.5,1]");
            expectFloat(uAt(right, 0, 0), 0.125f, "swapped right half rescales from [0,0.5]");
        }

        // Invalid texels (NaN / out-of-range) are excluded from detection and
        // stay invalid after the split — they must never become valid samples.
        {
            float uCols[8] = {0.0625f, 0.1875f, 0.3125f, 0.4375f,
                              0.5625f, 0.6875f, 0.8125f, 0.9375f};
            ndi_stmap::STMapImage packed;
            packed.width = W; packed.height = H; packed.uv = makePacked(uCols);
            packed.uv[0] = std::nanf("");   // (0,0) u = NaN
            packed.uv[2 * 2] = 1.5f;        // (2,0) u out of range
            ndi_stmap::STMapImage left, right;
            PackedHalfCoords cl, cr;
            std::string err;
            bool ok = ndi_stmap::splitPackedSTMap(packed, &left, &right, &cl, &cr, &err);
            const float nanU = uAt(left, 0, 0);
            const float bigU = uAt(left, 2, 0);
            check(ok && cl == PackedHalfCoords::PackedLeftHalf,
                  "invalid texels don't disturb packed-frame detection");
            check(!(nanU >= 0.0f && nanU <= 1.0f) && !(bigU >= 0.0f && bigU <= 1.0f),
                  "invalid texels stay invalid after the rescale");
        }

        // A half with no valid texels at all: copied verbatim, labeled so.
        {
            const float uCols[8] = {2.0f, -1.0f, 3.0f, 2.5f,
                                    0.125f, 0.375f, 0.625f, 0.875f};
            ndi_stmap::STMapImage packed;
            packed.width = W; packed.height = H; packed.uv = makePacked(uCols);
            ndi_stmap::STMapImage left, right;
            PackedHalfCoords cl, cr;
            std::string err;
            bool ok = ndi_stmap::splitPackedSTMap(packed, &left, &right, &cl, &cr, &err);
            check(ok && cl == PackedHalfCoords::NoValidTexels && cr == PackedHalfCoords::PerEye,
                  "all-invalid half labeled NoValidTexels, copied verbatim");
            expectFloat(uAt(left, 3, 0), 2.5f, "all-invalid half values unchanged");
        }

        // A packed-frame identity map splits into two per-eye identity maps —
        // the property that makes one Canon packed file equivalent to two
        // per-eye files.
        {
            const int pw = 16, ph = 4;
            std::vector<float> uv(static_cast<size_t>(pw) * ph * 2);
            for (int row = 0; row < ph; ++row) {
                const float v = (ph - 1 - row + 0.5f) / ph;
                for (int x = 0; x < pw; ++x) {
                    uv[(static_cast<size_t>(row) * pw + x) * 2 + 0] = (x + 0.5f) / pw;
                    uv[(static_cast<size_t>(row) * pw + x) * 2 + 1] = v;
                }
            }
            ndi_stmap::STMapImage packed;
            packed.width = pw; packed.height = ph; packed.uv = uv;
            ndi_stmap::STMapImage left, right;
            PackedHalfCoords cl, cr;
            std::string err;
            bool ok = ndi_stmap::splitPackedSTMap(packed, &left, &right, &cl, &cr, &err);
            std::vector<float> ident = identityMapUV(pw / 2, ph);
            check(ok &&
                      std::memcmp(left.uv.data(), ident.data(), ident.size() * 4) == 0 &&
                      std::memcmp(right.uv.data(), ident.data(), ident.size() * 4) == 0,
                  "packed-frame identity map splits into two per-eye identity maps");
        }

        // Odd packed width can't split into two equal eyes.
        {
            ndi_stmap::STMapImage packed;
            packed.width = 7; packed.height = 2;
            packed.uv.assign(7 * 2 * 2, 0.5f);
            ndi_stmap::STMapImage left, right;
            PackedHalfCoords cl, cr;
            std::string err;
            check(!ndi_stmap::splitPackedSTMap(packed, &left, &right, &cl, &cr, &err) &&
                      !err.empty(),
                  "odd packed width fails soft with an error message");
        }
    }

    if (failures) {
        std::fprintf(stderr, "%d test(s) FAILED\n", failures);
        return 1;
    }
    std::printf("All STMap tests passed\n");
    return 0;
}
