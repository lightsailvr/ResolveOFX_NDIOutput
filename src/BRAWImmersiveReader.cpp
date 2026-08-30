// Implementation of src/BRAWImmersiveReader.h — see that header for the
// contract. Compiles the vendored BRAW dispatch shim into this one TU; the
// shim locates BlackmagicRawAPI.framework at runtime (host bundle first, so
// inside Resolve it binds Resolve's own copy).
#ifdef __APPLE__

#include "BRAWImmersiveReader.h"

#include "BlackmagicRawAPI.h"
#include "BlackmagicRawAPIDispatch.cpp"

#include <CoreFoundation/CoreFoundation.h>

#include <cstdio>
#include <vector>

namespace ndi_brawreader {

namespace {

std::string cfToStd(CFStringRef s)
{
    if (s == nullptr) return std::string();
    const CFIndex length = CFStringGetLength(s);
    const CFIndex maxBytes = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
    std::vector<char> buffer(static_cast<size_t>(maxBytes), 0);
    if (!CFStringGetCString(s, buffer.data(), maxBytes, kCFStringEncodingUTF8)) {
        return std::string();
    }
    return std::string(buffer.data());
}

std::string hresultHex(HRESULT hr)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08x", static_cast<unsigned>(hr));
    return std::string(buf);
}

// Fetch one string-typed immersive attribute; empty optional-style return
// (false) only for genuinely absent/mistyped values.
bool getStringAttribute(IBlackmagicRawClipImmersiveVideo* immersive,
                        BlackmagicRawImmersiveAttribute attribute,
                        std::string* out)
{
    Variant value;
    VariantInit(&value);
    if (immersive->GetImmersiveAttribute(attribute, &value) != S_OK) {
        return false;
    }
    bool ok = false;
    if (value.vt == blackmagicRawVariantTypeString) {
        *out = cfToStd(value.bstrVal);
        ok = !out->empty();
    }
    VariantClear(&value);
    return ok;
}

} // namespace

bool readImmersiveCalibration(const std::string& brawPath,
                              std::string* jsonOut,
                              std::string* projectionKindOut,
                              std::string* calibrationTypeOut,
                              std::string* error)
{
    const auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        return false;
    };

    // Host bundle first (inside Resolve this binds Resolve's own framework),
    // then exe-relative, then the standalone Blackmagic RAW SDK install. The
    // dispatch shim latches the first successful load process-wide, so
    // repeated calls are cheap.
    IBlackmagicRawFactory* factory = CreateBlackmagicRawFactoryInstance();
    if (factory == nullptr) {
        factory = CreateBlackmagicRawFactoryInstanceFromPath(
            CFSTR("/Applications/Blackmagic RAW/Blackmagic RAW SDK/Mac/Libraries"));
    }
    if (factory == nullptr) {
        return fail("BlackmagicRawAPI.framework not found (host bundle or Blackmagic RAW SDK)");
    }

    IBlackmagicRaw* codec = nullptr;
    IBlackmagicRawClip* clip = nullptr;
    IBlackmagicRawClipImmersiveVideo* immersive = nullptr;
    CFStringRef clipPath = nullptr;
    bool ok = false;

    do {
        HRESULT hr = factory->CreateCodec(&codec);
        if (hr != S_OK || codec == nullptr) {
            fail("BRAW codec creation failed (" + hresultHex(hr) + ")");
            break;
        }
        clipPath = CFStringCreateWithCString(nullptr, brawPath.c_str(), kCFStringEncodingUTF8);
        if (clipPath == nullptr) {
            fail("clip path is not valid UTF-8");
            break;
        }
        hr = codec->OpenClip(clipPath, &clip);
        if (hr != S_OK || clip == nullptr) {
            fail("cannot open '" + brawPath + "' as BRAW (" + hresultHex(hr) + ")");
            break;
        }
        if (clip->QueryInterface(IID_IBlackmagicRawClipImmersiveVideo,
                                 reinterpret_cast<void**>(&immersive)) != S_OK ||
            immersive == nullptr) {
            immersive = nullptr;
            fail("clip has no immersive calibration (URSA Cine Immersive BRAW required)");
            break;
        }
        std::string json;
        if (!getStringAttribute(immersive, blackmagicRawImmersiveAttributeOpticalProjectionData,
                                &json)) {
            fail("clip carries no OpticalProjectionData calibration blob");
            break;
        }
        if (jsonOut) *jsonOut = json;
        if (projectionKindOut) {
            getStringAttribute(immersive, blackmagicRawImmersiveAttributeOpticalProjectionKind,
                               projectionKindOut);
        }
        if (calibrationTypeOut) {
            getStringAttribute(immersive, blackmagicRawImmersiveAttributeOpticalCalibrationType,
                               calibrationTypeOut);
        }
        ok = true;
    } while (false);

    if (immersive) immersive->Release();
    if (clip) clip->Release();
    if (codec) codec->Release();
    factory->Release();
    if (clipPath) CFRelease(clipPath);
    return ok;
}

} // namespace ndi_brawreader

#endif // __APPLE__
