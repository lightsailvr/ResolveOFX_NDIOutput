// Implementation of src/BRAWImmersiveReader.h — see that header for the
// contract. Compiles the vendored BRAW dispatch shim into this one TU; the
// shim locates the BRAW runtime at runtime relative to the host (bundle on
// macOS, exe on Windows — inside Resolve either way it binds Resolve's own
// copy).
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

#elif defined(_WIN32)

// Windows half (ticket #26), mirroring the macOS block above through the same
// dispatch mechanism: BlackmagicRawAPI.h here is MIDL-generated at build time
// from the vendored SDK IDL (third_party/braw/Win/, see CMakeLists.txt), and
// the vendored dispatch shim resolves BlackmagicRawAPI.dll at runtime —
// exe-relative first, which inside Resolve's process is Resolve's own shipped
// DLL. The API speaks real COM types on this platform (BSTR paths, VARIANT
// attributes), so the only differences from macOS are the string plumbing and
// the fallback search paths for standalone processes like the unit tests.

#include "BRAWImmersiveReader.h"

#include "BlackmagicRawAPI.h"
#include "BlackmagicRawAPIDispatch.cpp"

#include <cstdio>
#include <string>

namespace ndi_brawreader {

namespace {

std::string bstrToUtf8(BSTR s)
{
    if (s == nullptr) return std::string();
    const int wideLen = static_cast<int>(SysStringLen(s));
    if (wideLen == 0) return std::string();
    const int len = WideCharToMultiByte(CP_UTF8, 0, s, wideLen, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return std::string();
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, wideLen, &out[0], len, nullptr, nullptr);
    return out;
}

// UTF-8 → BSTR; null for byte sequences that are not valid UTF-8 (the
// parameter seams promise wide filesystem calls on honestly-converted paths,
// spec decision 15 — never guess an ANSI code page).
BSTR utf8ToBstr(const std::string& s)
{
    if (s.empty()) return SysAllocString(L"");
    const int wideLen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                            s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (wideLen <= 0) return nullptr;
    BSTR out = SysAllocStringLen(nullptr, static_cast<UINT>(wideLen));
    if (out == nullptr) return nullptr;
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out, wideLen);
    return out;
}

std::string hresultHex(HRESULT hr)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08x", static_cast<unsigned>(hr));
    return std::string(buf);
}

// Fetch one string-typed immersive attribute; false only for genuinely
// absent/mistyped values. blackmagicRawVariantTypeString == VT_BSTR on this
// platform (the IDL aliases the OLE variant types).
bool getStringAttribute(IBlackmagicRawClipImmersiveVideo* immersive,
                        BlackmagicRawImmersiveAttribute attribute,
                        std::string* out)
{
    VARIANT value;
    VariantInit(&value);
    if (immersive->GetImmersiveAttribute(attribute, &value) != S_OK) {
        return false;
    }
    bool ok = false;
    if (value.vt == VT_BSTR) {
        *out = bstrToUtf8(value.bstrVal);
        ok = !out->empty();
    }
    VariantClear(&value);
    return ok;
}

// Resolve the BRAW runtime, latching process-wide inside the dispatch shim.
// CreateBlackmagicRawFactoryInstance tries exe-relative "BlackmagicRawAPI\"
// then the exe's own folder — inside Resolve the latter is
// <Resolve dir>\BlackmagicRawAPI.dll, Resolve's shipped copy, so user
// machines need no separate Blackmagic install. The explicit paths exist
// for standalone processes (the unit-test binary): the installed Blackmagic
// RAW SDK, then a default-location Resolve. Inside Resolve they engage only
// if the host's own copy is missing or unloadable — a broken install —
// where a separately-installed SDK of a different version beats losing the
// feature (metadata-only reads; the attribute API is stable across 5.x).
IBlackmagicRawFactory* createFactory()
{
    IBlackmagicRawFactory* factory = CreateBlackmagicRawFactoryInstance();
    if (factory != nullptr) return factory;

    static const wchar_t* kFallbackDirs[] = {
        L"C:\\Program Files (x86)\\Blackmagic Design\\Blackmagic RAW\\"
        L"Blackmagic RAW SDK\\Win\\Libraries",
        L"C:\\Program Files\\Blackmagic Design\\DaVinci Resolve",
    };
    for (const wchar_t* dir : kFallbackDirs) {
        BSTR path = SysAllocString(dir);
        if (path == nullptr) continue;
        factory = CreateBlackmagicRawFactoryInstanceFromPath(path);
        SysFreeString(path);
        if (factory != nullptr) return factory;
    }
    return nullptr;
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

    // Courtesy COM init, as the SDK samples do. Inside Resolve the calling
    // (main) thread already has an apartment — RPC_E_CHANGED_MODE is fine,
    // the API doesn't care which mode; only balance an init we performed.
    const HRESULT coInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool coInitialized = (coInit == S_OK || coInit == S_FALSE);

    bool ok = false;
    IBlackmagicRawFactory* factory = nullptr;
    IBlackmagicRaw* codec = nullptr;
    IBlackmagicRawClip* clip = nullptr;
    IBlackmagicRawClipImmersiveVideo* immersive = nullptr;
    BSTR clipPath = nullptr;

    do {
        factory = createFactory();
        if (factory == nullptr) {
            fail("BlackmagicRawAPI.dll not found (host application or Blackmagic RAW SDK)");
            break;
        }
        HRESULT hr = factory->CreateCodec(&codec);
        if (hr != S_OK || codec == nullptr) {
            fail("BRAW codec creation failed (" + hresultHex(hr) + ")");
            break;
        }
        clipPath = utf8ToBstr(brawPath);
        if (clipPath == nullptr) {
            fail("clip path is not valid UTF-8");
            break;
        }
        hr = codec->OpenClip(clipPath, &clip);
        if (hr != S_OK || clip == nullptr) {
            fail("cannot open '" + brawPath + "' as BRAW (" + hresultHex(hr) + ")");
            break;
        }
        if (clip->QueryInterface(__uuidof(IBlackmagicRawClipImmersiveVideo),
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
    if (factory) factory->Release();
    if (clipPath) SysFreeString(clipPath);
    if (coInitialized) CoUninitialize();
    return ok;
}

} // namespace ndi_brawreader

#endif // __APPLE__ / _WIN32
