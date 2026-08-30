// braw_immersive_dump — dump general + immersive metadata from a BRAW clip.
// Built against Blackmagic RAW SDK 5.1 (Mac).
#include "BlackmagicRawAPI.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static std::string cfToStd(CFStringRef s)
{
    if (s == nullptr) return "(null)";
    CFIndex len = CFStringGetLength(s);
    CFIndex maxBytes = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
    std::vector<char> buf(maxBytes, 0);
    if (!CFStringGetCString(s, buf.data(), maxBytes, kCFStringEncodingUTF8))
        return "(unconvertible)";
    return std::string(buf.data());
}

static void printVariant(const Variant& value, const char* dumpDir, const char* keyName)
{
    switch (value.vt)
    {
        case blackmagicRawVariantTypeU8:      printf("u8(as s16): %d", (int)value.iVal); break;
        case blackmagicRawVariantTypeS16:     printf("s16: %d", (int)value.iVal); break;
        case blackmagicRawVariantTypeU16:     printf("u16: %u", (unsigned)value.uiVal); break;
        case blackmagicRawVariantTypeS32:     printf("s32: %d", value.intVal); break;
        case blackmagicRawVariantTypeU32:     printf("u32: %u", value.uintVal); break;
        case blackmagicRawVariantTypeFloat32: printf("f32: %g", value.fltVal); break;
        case blackmagicRawVariantTypeString:
        {
            std::string s = cfToStd(value.bstrVal);
            if (s.size() > 200) {
                printf("string[%zu bytes]: %.200s ... (truncated)", s.size(), s.c_str());
                // Save full value to a file for analysis
                std::string path = std::string(dumpDir) + "/" + keyName + ".txt";
                FILE* f = fopen(path.c_str(), "wb");
                if (f) { fwrite(s.data(), 1, s.size(), f); fclose(f); printf(" [saved to %s]", path.c_str()); }
            } else {
                printf("string: %s", s.c_str());
            }
            break;
        }
        case blackmagicRawVariantTypeSafeArray:
        {
            SafeArray* sa = value.parray;
            void* data = nullptr;
            BlackmagicRawVariantType at;
            long lo = 0, hi = -1;
            if (SafeArrayAccessData(sa, &data) != S_OK ||
                SafeArrayGetVartype(sa, &at) != S_OK ||
                SafeArrayGetLBound(sa, 1, &lo) != S_OK ||
                SafeArrayGetUBound(sa, 1, &hi) != S_OK) { printf("(safeArray unreadable)"); break; }
            long n = hi - lo + 1;
            printf("array[%ld] vt=%u: ", n, at);
            long shown = n > 16 ? 16 : n;
            for (long i = 0; i < shown; i++) {
                switch (at) {
                    case blackmagicRawVariantTypeU8:      printf("%u ", (unsigned)((unsigned char*)data)[i]); break;
                    case blackmagicRawVariantTypeS16:     printf("%d ", (int)((short*)data)[i]); break;
                    case blackmagicRawVariantTypeU16:     printf("%u ", (unsigned)((unsigned short*)data)[i]); break;
                    case blackmagicRawVariantTypeS32:     printf("%d ", ((int*)data)[i]); break;
                    case blackmagicRawVariantTypeU32:     printf("%u ", ((unsigned*)data)[i]); break;
                    case blackmagicRawVariantTypeFloat32: printf("%g ", ((float*)data)[i]); break;
                    default: printf("? "); break;
                }
            }
            if (n > shown) printf("... (%ld more)", n - shown);
            // Save raw bytes of big arrays for analysis
            if (n > 16) {
                size_t elemSize = (at == blackmagicRawVariantTypeU8) ? 1 :
                                  (at == blackmagicRawVariantTypeS16 || at == blackmagicRawVariantTypeU16) ? 2 : 4;
                std::string path = std::string(dumpDir) + "/" + keyName + ".bin";
                FILE* f = fopen(path.c_str(), "wb");
                if (f) { fwrite(data, elemSize, n, f); fclose(f); printf(" [saved to %s]", path.c_str()); }
            }
            SafeArrayUnaccessData(sa);
            break;
        }
        default: printf("(vt=%u unhandled)", value.vt); break;
    }
}

static void dumpIterator(const char* header, IBlackmagicRawMetadataIterator* it, const char* dumpDir)
{
    printf("\n===== %s =====\n", header);
    CFStringRef key = nullptr;
    while (SUCCEEDED(it->GetKey(&key)))
    {
        std::string keyName = cfToStd(key);
        printf("%s = ", keyName.c_str());
        Variant value;
        VariantInit(&value);
        if (it->GetData(&value) == S_OK) {
            printVariant(value, dumpDir, keyName.c_str());
            VariantClear(&value);
        } else {
            printf("(GetData failed)");
        }
        printf("\n");
        it->Next();
    }
}

class Callback : public IBlackmagicRawCallback
{
public:
    IBlackmagicRawFrame* frame = nullptr;
    virtual void ReadComplete(IBlackmagicRawJob*, HRESULT result, IBlackmagicRawFrame* f)
    {
        if (result == S_OK) { frame = f; frame->AddRef(); }
        else fprintf(stderr, "ReadComplete failed: 0x%08x\n", (unsigned)result);
    }
    virtual void ProcessComplete(IBlackmagicRawJob*, HRESULT, IBlackmagicRawProcessedImage*) {}
    virtual void DecodeComplete(IBlackmagicRawJob*, HRESULT) {}
    virtual void TrimProgress(IBlackmagicRawJob*, float) {}
    virtual void TrimComplete(IBlackmagicRawJob*, HRESULT) {}
    virtual void SidecarMetadataParseWarning(IBlackmagicRawClip*, CFStringRef, uint32_t, CFStringRef) {}
    virtual void SidecarMetadataParseError(IBlackmagicRawClip*, CFStringRef, uint32_t, CFStringRef) {}
    virtual void PreparePipelineComplete(void*, HRESULT) {}
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, LPVOID*) { return E_NOTIMPL; }
    virtual ULONG STDMETHODCALLTYPE AddRef() { return ++refs; }
    virtual ULONG STDMETHODCALLTYPE Release() { ULONG n = --refs; if (n == 0) delete this; return n; }
private:
    std::atomic<uint32_t> refs{0};
};

int main(int argc, char** argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s clip.braw dumpDir\n", argv[0]);
        return 1;
    }
    const char* clipPath = argv[1];
    const char* dumpDir = argv[2];

    CFStringRef clipName = CFStringCreateWithCString(nullptr, clipPath, kCFStringEncodingUTF8);
    CFStringRef libPath = CFSTR("/Applications/Blackmagic RAW/Blackmagic RAW SDK/Mac/Libraries");

    IBlackmagicRawFactory* factory = CreateBlackmagicRawFactoryInstanceFromPath(libPath);
    if (!factory) { fprintf(stderr, "no factory\n"); return 1; }

    IBlackmagicRaw* codec = nullptr;
    if (factory->CreateCodec(&codec) != S_OK) { fprintf(stderr, "no codec\n"); return 1; }

    IBlackmagicRawClip* clip = nullptr;
    HRESULT hr = codec->OpenClip(clipName, &clip);
    if (hr != S_OK) { fprintf(stderr, "OpenClip failed: 0x%08x\n", (unsigned)hr); return 1; }

    uint32_t w = 0, h = 0; float fps = 0; uint64_t frames = 0;
    clip->GetWidth(&w); clip->GetHeight(&h); clip->GetFrameRate(&fps); clip->GetFrameCount(&frames);
    printf("Clip: %s\n  reported WxH: %ux%u  fps: %g  frames: %llu\n", clipPath, w, h, fps, (unsigned long long)frames);

    // ---- Multi-video info ----
    IBlackmagicRawClipMultiVideo* multi = nullptr;
    if (clip->QueryInterface(IID_IBlackmagicRawClipMultiVideo, (void**)&multi) == S_OK && multi) {
        uint32_t tracks = 0;
        multi->GetVideoTrackCount(&tracks);
        printf("\n===== MultiVideo =====\ntrack count: %u\n", tracks);
        for (uint32_t t = 0; t < tracks; t++) {
            uint64_t fc = 0; uint32_t fourcc = 0;
            multi->GetVideoFrameCount(t, &fc);
            multi->GetVideoTrackSource(t, &fourcc);
            char cc[5] = { (char)(fourcc >> 24), (char)(fourcc >> 16), (char)(fourcc >> 8), (char)fourcc, 0 };
            printf("track %u: frames=%llu sourceFourCC='%s' (0x%08x)\n", t, (unsigned long long)fc, cc, fourcc);
        }
        multi->Release();
    } else {
        printf("\n(no IBlackmagicRawClipMultiVideo)\n");
    }

    // ---- Immersive info ----
    IBlackmagicRawClipImmersiveVideo* imm = nullptr;
    if (clip->QueryInterface(IID_IBlackmagicRawClipImmersiveVideo, (void**)&imm) == S_OK && imm) {
        printf("\n===== ImmersiveVideo =====\n");
        uint32_t dbl = 0, hfov = 0; int32_t cda = 0;
        if (imm->GetDistanceBetweenLenses(&dbl) == S_OK) printf("DistanceBetweenLenses: %u\n", dbl);
        if (imm->GetComfortDisparityAdjustment(&cda) == S_OK) printf("ComfortDisparityAdjustment: %d\n", cda);
        if (imm->GetHorizontalFieldOfView(&hfov) == S_OK) printf("HorizontalFieldOfView: %u\n", hfov);

        struct { BlackmagicRawImmersiveAttribute attr; const char* name; } attrs[] = {
            { blackmagicRawImmersiveAttributeOpticalLensProcessingDataFileUUID, "OpticalLensProcessingDataFileUUID" },
            { blackmagicRawImmersiveAttributeOpticalILPDFileName,               "OpticalILPDFileName" },
            { blackmagicRawImmersiveAttributeOpticalInteraxial,                 "OpticalInteraxial" },
            { blackmagicRawImmersiveAttributeOpticalProjectionKind,             "OpticalProjectionKind" },
            { blackmagicRawImmersiveAttributeOpticalCalibrationType,            "OpticalCalibrationType" },
            { blackmagicRawImmersiveAttributeOpticalProjectionData,             "OpticalProjectionData" },
        };
        for (auto& a : attrs) {
            Variant value;
            VariantInit(&value);
            HRESULT ahr = imm->GetImmersiveAttribute(a.attr, &value);
            printf("%s: ", a.name);
            if (ahr == S_OK) { printVariant(value, dumpDir, a.name); VariantClear(&value); }
            else printf("(failed 0x%08x)", (unsigned)ahr);
            printf("\n");
        }
        imm->Release();
    } else {
        printf("\n(no IBlackmagicRawClipImmersiveVideo)\n");
    }

    // ---- Clip metadata ----
    IBlackmagicRawMetadataIterator* clipIt = nullptr;
    if (clip->GetMetadataIterator(&clipIt) == S_OK && clipIt) {
        dumpIterator("Clip Metadata", clipIt, dumpDir);
        clipIt->Release();
    }

    // ---- Frame 0 metadata (via immersive read if plain read fails) ----
    Callback* cb = new Callback();
    cb->AddRef();
    codec->SetCallback(cb);

    IBlackmagicRawJob* job = nullptr;
    hr = clip->CreateJobReadFrame(0, &job);
    if (hr == S_OK && job) {
        hr = job->Submit();
        job->Release();
        if (hr == S_OK) codec->FlushJobs();
        else fprintf(stderr, "plain Submit failed: 0x%08x\n", (unsigned)hr);
    } else {
        fprintf(stderr, "plain CreateJobReadFrame failed: 0x%08x\n", (unsigned)hr);
    }

    if (!cb->frame) {
        IBlackmagicRawClipImmersiveVideo* imm2 = nullptr;
        if (clip->QueryInterface(IID_IBlackmagicRawClipImmersiveVideo, (void**)&imm2) == S_OK && imm2) {
            IBlackmagicRawJob* ijob = nullptr;
            if (imm2->CreateJobImmersiveReadFrame(blackmagicRawImmersiveVideoTrackLeft, 0, &ijob) == S_OK && ijob) {
                if (ijob->Submit() == S_OK) codec->FlushJobs();
                ijob->Release();
            }
            imm2->Release();
        }
    }

    if (cb->frame) {
        IBlackmagicRawMetadataIterator* frameIt = nullptr;
        if (cb->frame->GetMetadataIterator(&frameIt) == S_OK && frameIt) {
            dumpIterator("Frame 0 Metadata", frameIt, dumpDir);
            frameIt->Release();
        }
        cb->frame->Release();
        cb->frame = nullptr;
    } else {
        printf("\n(no frame 0 obtained — frame metadata skipped)\n");
    }

    cb->Release();
    clip->Release();
    codec->Release();
    factory->Release();
    CFRelease(clipName);
    return 0;
}
