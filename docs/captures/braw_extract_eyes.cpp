// braw_extract_eyes — decode one frame from each eye track of an immersive
// BRAW clip at quarter/eighth res, write PPMs for offline warp verification.
#include "BlackmagicRawAPI.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>

class Callback : public IBlackmagicRawCallback
{
public:
    IBlackmagicRawProcessedImage* processed = nullptr;

    virtual void ReadComplete(IBlackmagicRawJob* job, HRESULT result, IBlackmagicRawFrame* frame)
    {
        if (result != S_OK) { fprintf(stderr, "ReadComplete failed: 0x%08x\n", (unsigned)result); return; }
        frame->SetResolutionScale(blackmagicRawResolutionScaleQuarter);
        frame->SetResourceFormat(blackmagicRawResourceFormatRGBAU8);
        IBlackmagicRawJob* decodeJob = nullptr;
        if (frame->CreateJobDecodeAndProcessFrame(nullptr, nullptr, &decodeJob) == S_OK && decodeJob) {
            if (decodeJob->Submit() != S_OK) fprintf(stderr, "decode Submit failed\n");
            decodeJob->Release();
        } else {
            fprintf(stderr, "CreateJobDecodeAndProcessFrame failed\n");
        }
    }
    virtual void ProcessComplete(IBlackmagicRawJob*, HRESULT result, IBlackmagicRawProcessedImage* img)
    {
        if (result != S_OK) { fprintf(stderr, "ProcessComplete failed: 0x%08x\n", (unsigned)result); return; }
        processed = img;
        processed->AddRef();
    }
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

static bool writePPM(const char* path, const unsigned char* rgba, uint32_t w, uint32_t h)
{
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (uint32_t i = 0; i < w * h; i++) {
        fwrite(rgba + i * 4, 1, 3, f);
    }
    fclose(f);
    return true;
}

int main(int argc, char** argv)
{
    if (argc < 4) { fprintf(stderr, "usage: %s clip.braw frameIndex outPrefix\n", argv[0]); return 1; }
    const char* clipPath = argv[1];
    uint64_t frameIndex = strtoull(argv[2], nullptr, 10);
    std::string outPrefix = argv[3];

    CFStringRef clipName = CFStringCreateWithCString(nullptr, clipPath, kCFStringEncodingUTF8);
    IBlackmagicRawFactory* factory = CreateBlackmagicRawFactoryInstanceFromPath(
        CFSTR("/Applications/Blackmagic RAW/Blackmagic RAW SDK/Mac/Libraries"));
    if (!factory) { fprintf(stderr, "no factory\n"); return 1; }
    IBlackmagicRaw* codec = nullptr;
    if (factory->CreateCodec(&codec) != S_OK) { fprintf(stderr, "no codec\n"); return 1; }
    IBlackmagicRawClip* clip = nullptr;
    if (codec->OpenClip(clipName, &clip) != S_OK) { fprintf(stderr, "OpenClip failed\n"); return 1; }

    IBlackmagicRawClipImmersiveVideo* imm = nullptr;
    if (clip->QueryInterface(IID_IBlackmagicRawClipImmersiveVideo, (void**)&imm) != S_OK || !imm) {
        fprintf(stderr, "no immersive interface\n");
        return 1;
    }

    Callback* cb = new Callback();
    cb->AddRef();
    codec->SetCallback(cb);

    const BlackmagicRawImmersiveVideoTrack tracks[2] = {
        blackmagicRawImmersiveVideoTrackLeft, blackmagicRawImmersiveVideoTrackRight };
    const char* names[2] = { "left", "right" };

    for (int t = 0; t < 2; t++) {
        if (cb->processed) { cb->processed->Release(); cb->processed = nullptr; }
        IBlackmagicRawJob* job = nullptr;
        HRESULT hr = imm->CreateJobImmersiveReadFrame(tracks[t], frameIndex, &job);
        if (hr != S_OK || !job) { fprintf(stderr, "CreateJobImmersiveReadFrame(%s) failed: 0x%08x\n", names[t], (unsigned)hr); return 1; }
        hr = job->Submit();
        job->Release();
        if (hr != S_OK) { fprintf(stderr, "Submit(%s) failed: 0x%08x\n", names[t], (unsigned)hr); return 1; }
        codec->FlushJobs();

        if (!cb->processed) { fprintf(stderr, "no processed image for %s\n", names[t]); return 1; }
        uint32_t w = 0, h = 0, sz = 0;
        void* res = nullptr;
        cb->processed->GetWidth(&w);
        cb->processed->GetHeight(&h);
        cb->processed->GetResourceSizeBytes(&sz);
        cb->processed->GetResource(&res);
        printf("%s eye: %ux%u (%u bytes)\n", names[t], w, h, sz);
        std::string out = outPrefix + "_" + names[t] + ".ppm";
        if (!writePPM(out.c_str(), (const unsigned char*)res, w, h)) { fprintf(stderr, "PPM write failed\n"); return 1; }
        printf("wrote %s\n", out.c_str());
    }

    if (cb->processed) { cb->processed->Release(); cb->processed = nullptr; }
    cb->Release();
    imm->Release();
    clip->Release();
    codec->Release();
    factory->Release();
    CFRelease(clipName);
    return 0;
}
