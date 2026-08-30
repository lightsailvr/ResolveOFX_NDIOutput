#ifndef _BRAWLensMap_h_
#define _BRAWLensMap_h_

/*
  Camera-metadata projection maps (issue #11): URSA Cine Immersive BRAW clips
  embed a per-camera Apple Immersive calibration (attribute
  OpticalProjectionData, a JSON blob written by the "Apple Immersive Camera
  Calibration Service"). This header turns that JSON into the same per-eye
  ndi_stmap::STMapImage the Equirect (STMap) pipeline already consumes — the
  metadata is just a second SOURCE of maps, everything downstream (validation,
  Metal upload, warp, packing) is untouched.

  Kept free of BRAW-SDK, OFX, NDI, and Metal dependencies so it is
  unit-testable without a host or the SDK installed (tests/test_brawmap.cpp,
  fixture tests/fixtures/ursa_immersive_calibration.json). The SDK-facing
  extraction of the JSON from a .braw lives in src/BRAWImmersiveReader.cpp.

  Geometry (verified numerically and visually against real media —
  docs/2026-08-30-braw-immersive-metadata-projection.md):

  - Intrinsics model "radial2ProjectionOffsetTangential2" is the Mei-Rives
    unified fisheye model, distortions ordered [k1, k2, xi, p1, p2], camera
    frame x-right / y-down / z-forward:
        m  = (x/(z+xi), y/(z+xi));  rho2 = |m|^2;  L = 1 + k1 rho2 + k2 rho2^2
        xd = L mx + 2 p1 mx my + p2 (rho2 + 2 mx^2)
        yd = L my + p1 (rho2 + 2 my^2) + 2 p2 mx my
        u_px = fx xd + skew yd + cx;   v_px = fy yd + cy
    valid while z+xi > 0 and the ray is within calibrationLimitRadialAngle
    of the axis. (Cross-check: a ray at half the stated horizontal FOV lands
    on the sensor edge to 0.1%.)
  - Extrinsics "dualRectification" quaternions are rig-to-camera (the CV
    world-to-camera convention — the only reading consistent with the right
    eye's translation (-64,0,0) mm given x-right): d_cam = R(quat) · d_rig.
    The two candidate conventions differ by ~0.04° between eyes (sub-pixel at
    any stream size); if a device cross-check ever proves this wrong the fix
    is transposing quatToMatrix.
  - Each output map is a per-eye 180°x180° equirect (the clip-level
    presentation FOV): texel (r top-down, c) → lon = ((c+.5)/size - .5)·180°
    (+right), lat = (.5 - (r+.5)/size)·180° (+up), direction in the rig frame
    (cos lat sin lon, -sin lat, cos lat cos lon). Projected pixels normalize
    to STMap convention: u = (u_px+.5)/W, v = 1 - (v_px+.5)/H (v=0 = source
    bottom, matching the EXR loader's output). Geometrically invalid texels
    get (-1,-1) — the warp's existing "outside the lens" handling.
  - maskData is the visionOS porthole: control points are unit vectors in a
    y-up/z-BACKWARD frame, all (on this camera) exactly 80° off the -z axis.
    Mask ON marks texels whose RIG-space direction exceeds the mean
    control-point angle invalid — a hard cut at the circle (the 2.5° linear
    feather needs a weight channel through both warp paths; deferred).

  The JSON parser below is a minimal, bounds-checked, locale-independent
  recursive-descent parser in the spirit of the STMap EXR reader: hostile or
  truncated input fails soft with an error string, never crashes.
*/

#include "STMap.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace ndi_brawmap {

// Caps applied before any allocation (same philosophy as STMap.h).
constexpr size_t kMaxJsonBytes = 8u << 20;  // calibration blobs run ~30 KB
constexpr int kMaxJsonDepth = 64;
constexpr int kMinMapSize = 16;
constexpr int kMaxMapSize = 8192;

namespace detail_json {

struct Value {
    enum Type { kNull, kBool, kNumber, kString, kArray, kObject };
    Type type = kNull;
    bool boolean = false;
    double number = 0.0;
    std::string str;
    std::vector<Value> items;                            // kArray
    std::vector<std::pair<std::string, Value>> members;  // kObject (order kept)

    const Value* find(const char* key) const
    {
        if (type != kObject) return nullptr;
        for (const auto& kv : members) {
            if (kv.first == key) return &kv.second;
        }
        return nullptr;
    }
};

struct Parser {
    const char* p;
    const char* end;
    int depth = 0;
    std::string* error;

    bool fail(const std::string& msg)
    {
        if (error && error->empty()) *error = "calibration JSON: " + msg;
        return false;
    }

    void skipWs()
    {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    }

    bool parseString(std::string* out)
    {
        out->clear();
        if (p >= end || *p != '"') return fail("expected string");
        ++p;
        while (p < end) {
            const char c = *p++;
            if (c == '"') return true;
            if (c == '\\') {
                if (p >= end) break;
                const char e = *p++;
                switch (e) {
                    case '"': out->push_back('"'); break;
                    case '\\': out->push_back('\\'); break;
                    case '/': out->push_back('/'); break;
                    case 'b': out->push_back('\b'); break;
                    case 'f': out->push_back('\f'); break;
                    case 'n': out->push_back('\n'); break;
                    case 'r': out->push_back('\r'); break;
                    case 't': out->push_back('\t'); break;
                    case 'u': {
                        // Calibration keys/values are ASCII; decode BMP
                        // escapes to UTF-8 just enough to not fail on them.
                        if (end - p < 4) return fail("truncated \\u escape");
                        unsigned code = 0;
                        for (int i = 0; i < 4; ++i) {
                            const char h = *p++;
                            code <<= 4;
                            if (h >= '0' && h <= '9') code |= static_cast<unsigned>(h - '0');
                            else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned>(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned>(h - 'A' + 10);
                            else return fail("bad \\u escape");
                        }
                        if (code < 0x80) {
                            out->push_back(static_cast<char>(code));
                        } else if (code < 0x800) {
                            out->push_back(static_cast<char>(0xC0 | (code >> 6)));
                            out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
                        } else {
                            out->push_back(static_cast<char>(0xE0 | (code >> 12)));
                            out->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                            out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
                        }
                        break;
                    }
                    default: return fail("unknown escape");
                }
            } else {
                out->push_back(c);
            }
            if (out->size() > kMaxJsonBytes) return fail("string too long");
        }
        return fail("unterminated string");
    }

    // Locale-independent number parse (strtod honors LC_NUMERIC — a host
    // process in a comma-decimal locale would silently break it).
    bool parseNumber(double* out)
    {
        bool neg = false;
        if (p < end && (*p == '-' || *p == '+')) {
            neg = (*p == '-');
            ++p;
        }
        if (p >= end || !((*p >= '0' && *p <= '9') || *p == '.')) return fail("expected number");
        double mant = 0.0;
        int fracDigits = 0;
        bool any = false;
        while (p < end && *p >= '0' && *p <= '9') {
            mant = mant * 10.0 + (*p - '0');
            ++p;
            any = true;
        }
        if (p < end && *p == '.') {
            ++p;
            while (p < end && *p >= '0' && *p <= '9') {
                mant = mant * 10.0 + (*p - '0');
                ++fracDigits;
                ++p;
                any = true;
            }
        }
        if (!any) return fail("malformed number");
        int expo = 0;
        if (p < end && (*p == 'e' || *p == 'E')) {
            ++p;
            bool expNeg = false;
            if (p < end && (*p == '-' || *p == '+')) {
                expNeg = (*p == '-');
                ++p;
            }
            if (p >= end || *p < '0' || *p > '9') return fail("malformed exponent");
            while (p < end && *p >= '0' && *p <= '9') {
                if (expo < 100000) expo = expo * 10 + (*p - '0');
                ++p;
            }
            if (expNeg) expo = -expo;
        }
        const double scale = std::pow(10.0, static_cast<double>(expo - fracDigits));
        *out = (neg ? -mant : mant) * scale;
        return true;
    }

    bool parseValue(Value* out)
    {
        if (++depth > kMaxJsonDepth) return fail("nesting too deep");
        skipWs();
        if (p >= end) {
            --depth;
            return fail("truncated");
        }
        bool ok = false;
        const char c = *p;
        if (c == '{') {
            ++p;
            out->type = Value::kObject;
            skipWs();
            if (p < end && *p == '}') {
                ++p;
                ok = true;
            } else {
                for (;;) {
                    std::string key;
                    skipWs();
                    if (!parseString(&key)) break;
                    skipWs();
                    if (p >= end || *p != ':') {
                        fail("expected ':'");
                        break;
                    }
                    ++p;
                    out->members.emplace_back(std::move(key), Value());
                    if (!parseValue(&out->members.back().second)) break;
                    skipWs();
                    if (p < end && *p == ',') {
                        ++p;
                        continue;
                    }
                    if (p < end && *p == '}') {
                        ++p;
                        ok = true;
                    } else {
                        fail("expected ',' or '}'");
                    }
                    break;
                }
            }
        } else if (c == '[') {
            ++p;
            out->type = Value::kArray;
            skipWs();
            if (p < end && *p == ']') {
                ++p;
                ok = true;
            } else {
                for (;;) {
                    out->items.emplace_back();
                    if (!parseValue(&out->items.back())) break;
                    skipWs();
                    if (p < end && *p == ',') {
                        ++p;
                        continue;
                    }
                    if (p < end && *p == ']') {
                        ++p;
                        ok = true;
                    } else {
                        fail("expected ',' or ']'");
                    }
                    break;
                }
            }
        } else if (c == '"') {
            out->type = Value::kString;
            ok = parseString(&out->str);
        } else if (c == 't') {
            if (end - p >= 4 && std::memcmp(p, "true", 4) == 0) {
                out->type = Value::kBool;
                out->boolean = true;
                p += 4;
                ok = true;
            } else {
                fail("bad literal");
            }
        } else if (c == 'f') {
            if (end - p >= 5 && std::memcmp(p, "false", 5) == 0) {
                out->type = Value::kBool;
                out->boolean = false;
                p += 5;
                ok = true;
            } else {
                fail("bad literal");
            }
        } else if (c == 'n') {
            if (end - p >= 4 && std::memcmp(p, "null", 4) == 0) {
                out->type = Value::kNull;
                p += 4;
                ok = true;
            } else {
                fail("bad literal");
            }
        } else {
            out->type = Value::kNumber;
            ok = parseNumber(&out->number);
        }
        --depth;
        return ok;
    }
};

inline bool parse(const std::string& text, Value* out, std::string* error)
{
    if (text.size() > kMaxJsonBytes) {
        if (error) *error = "calibration JSON: over size cap";
        return false;
    }
    Parser parser{text.data(), text.data() + text.size(), 0, error};
    if (!parser.parseValue(out)) return false;
    parser.skipWs();
    if (parser.p != parser.end) {
        if (error && error->empty()) *error = "calibration JSON: trailing data";
        return false;
    }
    return true;
}

} // namespace detail_json

// ---------------------------------------------------------------------------
// Parsed calibration
// ---------------------------------------------------------------------------

struct LensView {
    double fx = 0, fy = 0, cx = 0, cy = 0, skew = 0;
    double k1 = 0, k2 = 0, xi = 0, p1 = 0, p2 = 0;
    double calibLimitDeg = 180.0;   // rays past this angle off-axis are invalid
    double quat[4] = {0, 0, 0, 1};  // x,y,z,w — rig-to-camera rotation
    int imageWidth = 0, imageHeight = 0;
    double maskRadiusDeg = -1.0;    // <=0: clip carries no usable mask
    double maskEdgeDeg = 0.0;       // feather width (informational for now)
    double maskSpreadDeg = 0.0;     // control-point angle spread; >1° means
                                    // the circle approximation is lossy
};

struct LensCalibration {
    LensView left, right;
    std::string generator;  // e.g. "Apple Immersive Camera Calibration Service"
};

namespace detail {

inline bool readViewNumber(const detail_json::Value& obj, const char* key, double* out)
{
    const detail_json::Value* v = obj.find(key);
    if (!v || v->type != detail_json::Value::kNumber) return false;
    *out = v->number;
    return true;
}

inline bool parseView(const detail_json::Value& viewObj, LensView* out, std::string* error)
{
    const auto fail = [&](const std::string& msg) {
        if (error && error->empty()) *error = msg;
        return false;
    };

    const detail_json::Value* intr = viewObj.find("intrinsics");
    if (!intr || intr->type != detail_json::Value::kObject) {
        return fail("view has no intrinsics object");
    }
    const detail_json::Value* model = intr->find("model");
    if (!model || model->type != detail_json::Value::kString) {
        return fail("intrinsics has no model string");
    }
    if (model->str != "radial2ProjectionOffsetTangential2") {
        return fail("unsupported lens model '" + model->str +
                    "' (need radial2ProjectionOffsetTangential2)");
    }
    if (!readViewNumber(*intr, "fx", &out->fx) || !readViewNumber(*intr, "fy", &out->fy) ||
        !readViewNumber(*intr, "centerX", &out->cx) || !readViewNumber(*intr, "centerY", &out->cy)) {
        return fail("intrinsics is missing fx/fy/centerX/centerY");
    }
    readViewNumber(*intr, "skew", &out->skew);  // optional, defaults 0
    const detail_json::Value* dist = intr->find("distortions");
    if (!dist || dist->type != detail_json::Value::kArray || dist->items.size() < 5) {
        return fail("intrinsics needs a 5-element distortions array [k1,k2,xi,p1,p2]");
    }
    double d[5];
    for (int i = 0; i < 5; ++i) {
        if (dist->items[static_cast<size_t>(i)].type != detail_json::Value::kNumber) {
            return fail("distortions array holds a non-number");
        }
        d[i] = dist->items[static_cast<size_t>(i)].number;
    }
    out->k1 = d[0];
    out->k2 = d[1];
    out->xi = d[2];
    out->p1 = d[3];
    out->p2 = d[4];
    double limit = 0;
    if (readViewNumber(*intr, "calibrationLimitRadialAngle", &limit) && limit > 0) {
        out->calibLimitDeg = limit;
    }
    if (out->fx <= 0 || out->fy <= 0) {
        return fail("non-positive focal length in intrinsics");
    }

    const detail_json::Value* imageSize = viewObj.find("imageSize");
    if (!imageSize || imageSize->type != detail_json::Value::kArray ||
        imageSize->items.size() < 2 ||
        imageSize->items[0].type != detail_json::Value::kNumber ||
        imageSize->items[1].type != detail_json::Value::kNumber) {
        return fail("view has no imageSize [w,h]");
    }
    out->imageWidth = static_cast<int>(imageSize->items[0].number);
    out->imageHeight = static_cast<int>(imageSize->items[1].number);
    if (out->imageWidth < 1 || out->imageHeight < 1 ||
        out->imageWidth > 65536 || out->imageHeight > 65536) {
        return fail("imageSize out of range");
    }

    // Extrinsics are optional (identity = unrectified); when present the
    // quaternion must be well-formed.
    const detail_json::Value* extr = viewObj.find("extrinsics");
    if (extr && extr->type == detail_json::Value::kArray && !extr->items.empty()) {
        const detail_json::Value* quat = extr->items[0].find("quat");
        if (!quat || quat->type != detail_json::Value::kArray || quat->items.size() < 4) {
            return fail("extrinsics[0] has no quat[4]");
        }
        double q[4];
        double norm2 = 0;
        for (int i = 0; i < 4; ++i) {
            if (quat->items[static_cast<size_t>(i)].type != detail_json::Value::kNumber) {
                return fail("quat holds a non-number");
            }
            q[i] = quat->items[static_cast<size_t>(i)].number;
            norm2 += q[i] * q[i];
        }
        if (!(norm2 > 1e-12)) {
            return fail("degenerate extrinsics quaternion");
        }
        for (int i = 0; i < 4; ++i) out->quat[i] = q[i];
    }

    // maskData is optional; when present, reduce the control-point ring to a
    // circle (angle off the mask frame's -z axis — visionOS looks down -z).
    const detail_json::Value* maskData = viewObj.find("maskData");
    if (maskData && maskData->type == detail_json::Value::kObject) {
        readViewNumber(*maskData, "edgeWidth", &out->maskEdgeDeg);
        const detail_json::Value* params = maskData->find("maskViewParameters");
        const detail_json::Value* points = params ? params->find("controlPoints") : nullptr;
        if (points && points->type == detail_json::Value::kArray && !points->items.empty()) {
            double sum = 0, minAngle = 1e9, maxAngle = -1e9;
            size_t counted = 0;
            for (const detail_json::Value& pt : points->items) {
                if (pt.type != detail_json::Value::kArray || pt.items.size() < 3 ||
                    pt.items[2].type != detail_json::Value::kNumber) {
                    continue;
                }
                double z = pt.items[2].number;
                if (z < -1.0) z = -1.0;
                if (z > 1.0) z = 1.0;
                const double angle = std::acos(-z) * (180.0 / M_PI);
                sum += angle;
                if (angle < minAngle) minAngle = angle;
                if (angle > maxAngle) maxAngle = angle;
                ++counted;
            }
            if (counted > 0) {
                out->maskRadiusDeg = sum / static_cast<double>(counted);
                out->maskSpreadDeg = maxAngle - minAngle;
            }
        }
    }
    return true;
}

} // namespace detail

// Parse an OpticalProjectionData blob. Returns true and fills *out on
// success; any structural problem fails soft with a human-readable *error.
inline bool parseCalibrationJSON(const std::string& json, LensCalibration* out, std::string* error)
{
    detail_json::Value root;
    if (!detail_json::parse(json, &root, error)) return false;
    if (root.type != detail_json::Value::kObject) {
        if (error) *error = "calibration JSON: root is not an object";
        return false;
    }
    const detail_json::Value* generator = root.find("generator");
    if (generator && generator->type == detail_json::Value::kString) {
        out->generator = generator->str;
    }
    const detail_json::Value* captureDevice = root.find("captureDevice");
    const detail_json::Value* views =
        captureDevice ? captureDevice->find("views") : nullptr;
    if (!views || views->type != detail_json::Value::kArray) {
        if (error) *error = "calibration JSON has no captureDevice.views array";
        return false;
    }
    bool haveLeft = false, haveRight = false;
    for (const detail_json::Value& viewObj : views->items) {
        const detail_json::Value* desc = viewObj.find("viewDescription");
        if (!desc || desc->type != detail_json::Value::kString) continue;
        LensView* target = nullptr;
        if (desc->str == "left" && !haveLeft) {
            target = &out->left;
            haveLeft = true;
        } else if (desc->str == "right" && !haveRight) {
            target = &out->right;
            haveRight = true;
        }
        if (target && !detail::parseView(viewObj, target, error)) {
            return false;
        }
    }
    if (!haveLeft || !haveRight) {
        if (error) {
            *error = std::string("calibration JSON is missing the ") +
                     (haveLeft ? "right" : "left") + " view";
        }
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Mei-Rives projection + map generation
// ---------------------------------------------------------------------------

// Projects unit camera-space directions to calibration-pixel coordinates.
// Exposed (and used by generateEyeMap) so tests pin the projection itself.
struct MeiProjector {
    const LensView& view;
    double cosLimit;

    explicit MeiProjector(const LensView& v)
        : view(v), cosLimit(std::cos(v.calibLimitDeg * (M_PI / 180.0))) {}

    // d must be unit length; returns false past the calibration limit or the
    // projection singularity. u/v are pixels in imageWidth/imageHeight coords
    // (pixel-center-at-integer convention).
    bool project(double dx, double dy, double dz, double* uPx, double* vPx) const
    {
        if (dz < cosLimit) return false;
        const double denom = dz + view.xi;
        if (denom <= 1e-9) return false;
        const double mx = dx / denom;
        const double my = dy / denom;
        const double rho2 = mx * mx + my * my;
        const double radial = 1.0 + view.k1 * rho2 + view.k2 * rho2 * rho2;
        const double xd = radial * mx + 2.0 * view.p1 * mx * my +
                          view.p2 * (rho2 + 2.0 * mx * mx);
        const double yd = radial * my + view.p1 * (rho2 + 2.0 * my * my) +
                          2.0 * view.p2 * mx * my;
        *uPx = view.fx * xd + view.skew * yd + view.cx;
        *vPx = view.fy * yd + view.cy;
        return true;
    }
};

namespace detail {

inline void quatToMatrix(const double q[4], double R[3][3])
{
    const double norm = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    const double x = q[0] / norm, y = q[1] / norm, z = q[2] / norm, w = q[3] / norm;
    R[0][0] = 1 - 2 * (y * y + z * z);
    R[0][1] = 2 * (x * y - z * w);
    R[0][2] = 2 * (x * z + y * w);
    R[1][0] = 2 * (x * y + z * w);
    R[1][1] = 1 - 2 * (x * x + z * z);
    R[1][2] = 2 * (y * z - x * w);
    R[2][0] = 2 * (x * z - y * w);
    R[2][1] = 2 * (y * z + x * w);
    R[2][2] = 1 - 2 * (x * x + y * y);
}

} // namespace detail

// Generate one eye's 180°x180° equirect STMap (size x size texels) from its
// calibration. maskRadiusDeg <= 0 disables the mask regardless of applyMask.
// Fails soft (bad size, allocation failure) with an error string.
inline bool generateEyeMap(const LensView& view, int size, bool applyMask,
                           ndi_stmap::STMapImage* out, std::string* error)
{
    const auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        return false;
    };
    if (size < kMinMapSize || size > kMaxMapSize) {
        return fail("map size " + std::to_string(size) + " out of range");
    }
    if (view.imageWidth < 1 || view.imageHeight < 1) {
        return fail("calibration has no imageSize");
    }

    try {
        out->width = size;
        out->height = size;
        out->uv.assign(static_cast<size_t>(size) * size * 2, -1.0f);
    } catch (const std::bad_alloc&) {
        return fail("out of memory generating lens map");
    }

    double R[3][3];
    detail::quatToMatrix(view.quat, R);
    const MeiProjector projector(view);
    const bool maskOn = applyMask && view.maskRadiusDeg > 0.0;
    const double cosMask = maskOn ? std::cos(view.maskRadiusDeg * (M_PI / 180.0)) : -2.0;
    const double invW = 1.0 / static_cast<double>(view.imageWidth);
    const double invH = 1.0 / static_cast<double>(view.imageHeight);

    // Per-row/column trig hoisted out of the texel loop: the per-texel work
    // is then a 3x3 rotate plus the projection polynomial — ~30 ms at 2048²,
    // cheap enough for the parameter-edit path that calls this.
    std::vector<double> sinLon(static_cast<size_t>(size)), cosLon(static_cast<size_t>(size));
    for (int c = 0; c < size; ++c) {
        const double lon = ((c + 0.5) / static_cast<double>(size) - 0.5) * M_PI;
        sinLon[static_cast<size_t>(c)] = std::sin(lon);
        cosLon[static_cast<size_t>(c)] = std::cos(lon);
    }

    for (int r = 0; r < size; ++r) {
        const double lat = (0.5 - (r + 0.5) / static_cast<double>(size)) * M_PI;
        const double cosLat = std::cos(lat);
        const double sinLat = std::sin(lat);
        float* row = out->uv.data() + static_cast<size_t>(r) * size * 2;
        for (int c = 0; c < size; ++c) {
            const double rx = cosLat * sinLon[static_cast<size_t>(c)];
            const double ry = -sinLat;
            const double rz = cosLat * cosLon[static_cast<size_t>(c)];
            if (maskOn && rz < cosMask) continue;  // outside the porthole: stays (-1,-1)
            const double dx = R[0][0] * rx + R[0][1] * ry + R[0][2] * rz;
            const double dy = R[1][0] * rx + R[1][1] * ry + R[1][2] * rz;
            const double dz = R[2][0] * rx + R[2][1] * ry + R[2][2] * rz;
            double uPx = 0, vPx = 0;
            if (!projector.project(dx, dy, dz, &uPx, &vPx)) continue;
            row[static_cast<size_t>(c) * 2] = static_cast<float>((uPx + 0.5) * invW);
            row[static_cast<size_t>(c) * 2 + 1] = static_cast<float>(1.0 - (vPx + 0.5) * invH);
        }
    }
    return true;
}

// Generate both eyes' maps. All-or-nothing: on any failure neither output is
// meaningful and *error says why.
inline bool generateLensMaps(const LensCalibration& cal, int size, bool applyMask,
                             ndi_stmap::STMapImage* leftOut, ndi_stmap::STMapImage* rightOut,
                             std::string* error)
{
    if (!generateEyeMap(cal.left, size, applyMask, leftOut, error)) return false;
    return generateEyeMap(cal.right, size, applyMask, rightOut, error);
}

} // namespace ndi_brawmap

#endif
