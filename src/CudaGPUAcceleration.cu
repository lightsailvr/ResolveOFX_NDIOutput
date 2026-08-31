#ifdef _WIN32

// CUDA implementation of the GPU-module contract (Windows port ticket #22).
// See CudaGPUAcceleration.h for the seam documentation and
// MetalGPUAcceleration.mm for the prior art this mirrors.
//
// Byte-identity: every kernel reproduces the CPU reference composition
// (ndi_stream::downscaleRGBABox / ndi_stmap::warpRGBABox followed by the
// flipping converters) operation-for-operation, in the same order, and the
// translation unit compiles with -fmad=false (CMakeLists.txt) so nvcc cannot
// contract the multiply-adds MSVC leaves separate. tests/test_cuda_downscale
// holds the outputs to memcmp equality with the CPU oracles.
//
// Host discipline (spec decision 6): everything enqueues on the caller's
// stream; the only waits are cudaStreamSynchronize in the explicitly-blocking
// entry points (tests and the slow readback fallback). cudaDeviceSynchronize
// and cudaDeviceReset appear nowhere in this plugin.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cuda_runtime.h>

#include "CudaGPUAcceleration.h"

#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>

// Live capture via DebugView/WinDbg, same channel as the plugin's Windows
// NDI_LOG sink (the Metal module logs through its own os_log the same way).
static void cudaLog(const char* fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf(buf, sizeof(buf) - 2, fmt, args);
    va_end(args);
    if (n < 0) return;
    size_t len = static_cast<size_t>(n);
    if (len > sizeof(buf) - 2) len = sizeof(buf) - 2;
    buf[len] = '\n';
    buf[len + 1] = '\0';
    OutputDebugStringA(buf);
}
#define CUDA_LOG(fmt, ...) cudaLog("NDI Plugin CUDA: " fmt, ##__VA_ARGS__)

// ---------------------------------------------------------------------------
// Kernels. One thread emits one 4:2:2 macropixel (two output pixels), exactly
// like the Metal kernels; the arithmetic mirrors the CPU references (see the
// byte-identity note at the top of this file).
// ---------------------------------------------------------------------------

// One params struct for both kernel families (mapWidth/mapHeight unused by
// the plain downscale); passed by value, so no constant-buffer plumbing.
struct CudaConvertParams {
    unsigned srcWidth;
    unsigned srcHeight;
    unsigned srcRowFloats;   // source stride in floats (rowBytes / 4)
    unsigned outWidth;
    unsigned outHeight;
    unsigned divisor;
    unsigned mapWidth;
    unsigned mapHeight;
};

__device__ __forceinline__ float cudaClamp01(float v)
{
    // Same tree as the CPU converters: fmax(0, fmin(1, v)).
    return fmaxf(0.0f, fminf(1.0f, v));
}

// Fused box sample: matches ndi_stream::downscaleRGBABox composed with the
// flipping converters — output top-down row oy averages source bottom-up rows
// (outHeight-1-oy)*divisor + [0, divisor), edge-clamped, summed in (sy, sx)
// order (float addition order is part of the byte-identity contract).
__device__ __forceinline__ void cudaBoxSampleRGB(const float* src, const CudaConvertParams& p,
                                                 unsigned ox, unsigned oy, float rgb[3])
{
    float sum0 = 0.0f, sum1 = 0.0f, sum2 = 0.0f;
    for (unsigned sy = 0; sy < p.divisor; ++sy) {
        const unsigned srcY = min((p.outHeight - 1u - oy) * p.divisor + sy, p.srcHeight - 1u);
        const float* row = src + static_cast<size_t>(srcY) * p.srcRowFloats;
        for (unsigned sx = 0; sx < p.divisor; ++sx) {
            const unsigned srcX = min(ox * p.divisor + sx, p.srcWidth - 1u);
            const float* px = row + static_cast<size_t>(srcX) * 4u;
            sum0 += px[0];
            sum1 += px[1];
            sum2 += px[2];
        }
    }
    const float invCount = 1.0f / static_cast<float>(p.divisor * p.divisor);
    rgb[0] = cudaClamp01(sum0 * invCount);
    rgb[1] = cudaClamp01(sum1 * invCount);
    rgb[2] = cudaClamp01(sum2 * invCount);
}

// Bilinear source fetch mirroring ndi_stmap::detail::sampleBilinearClamped
// (rgb only): integer clamps on every index so even a hostile map value can
// never read out of bounds.
__device__ __forceinline__ void cudaWarpFetchRGB(const float* src, const CudaConvertParams& p,
                                                 float sx, float sy, float out[3])
{
    int x0 = static_cast<int>(floorf(sx));
    int y0 = static_cast<int>(floorf(sy));
    x0 = min(max(x0, 0), static_cast<int>(p.srcWidth) - 1);
    y0 = min(max(y0, 0), static_cast<int>(p.srcHeight) - 1);
    const int x1 = min(x0 + 1, static_cast<int>(p.srcWidth) - 1);
    const int y1 = min(y0 + 1, static_cast<int>(p.srcHeight) - 1);
    const float fx = fminf(fmaxf(sx - static_cast<float>(x0), 0.0f), 1.0f);
    const float fy = fminf(fmaxf(sy - static_cast<float>(y0), 0.0f), 1.0f);
    const float* p00 = src + static_cast<size_t>(y0) * p.srcRowFloats + static_cast<size_t>(x0) * 4u;
    const float* p10 = src + static_cast<size_t>(y0) * p.srcRowFloats + static_cast<size_t>(x1) * 4u;
    const float* p01 = src + static_cast<size_t>(y1) * p.srcRowFloats + static_cast<size_t>(x0) * 4u;
    const float* p11 = src + static_cast<size_t>(y1) * p.srcRowFloats + static_cast<size_t>(x1) * 4u;
    for (int c = 0; c < 3; ++c) {
        const float a = p00[c] + (p10[c] - p00[c]) * fx;
        const float b = p01[c] + (p11[c] - p01[c]) * fx;
        out[c] = a + (b - a) * fy;
    }
}

// Warped counterpart of cudaBoxSampleRGB, mirroring ndi_stmap::warpRGBABox:
// taps whose map value leaves [0,1] (or is NaN — the comparisons fail) are
// outside the lens image circle and stay black.
__device__ __forceinline__ void cudaWarpSampleRGB(const float* src, const float* mapUV,
                                                  const CudaConvertParams& p,
                                                  unsigned ox, unsigned oy, float rgb[3])
{
    float sum0 = 0.0f, sum1 = 0.0f, sum2 = 0.0f;
    for (unsigned sy = 0; sy < p.divisor; ++sy) {
        const unsigned dstY = min((p.outHeight - 1u - oy) * p.divisor + sy, p.mapHeight - 1u);
        const unsigned mapRow = p.mapHeight - 1u - dstY;
        for (unsigned sx = 0; sx < p.divisor; ++sx) {
            const unsigned dstX = min(ox * p.divisor + sx, p.mapWidth - 1u);
            const size_t mi = (static_cast<size_t>(mapRow) * p.mapWidth + dstX) * 2u;
            const float u = mapUV[mi];
            const float v = mapUV[mi + 1u];
            if (u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f) {
                float px[3];
                cudaWarpFetchRGB(src, p,
                                 u * static_cast<float>(p.srcWidth) - 0.5f,
                                 v * static_cast<float>(p.srcHeight) - 0.5f, px);
                sum0 += px[0];
                sum1 += px[1];
                sum2 += px[2];
            }
        }
    }
    const float invCount = 1.0f / static_cast<float>(p.divisor * p.divisor);
    rgb[0] = cudaClamp01(sum0 * invCount);
    rgb[1] = cudaClamp01(sum1 * invCount);
    rgb[2] = cudaClamp01(sum2 * invCount);
}

// Shared packing tails: two sampled pixels -> one UYVY macropixel / one P216
// column pair. Expression trees match the CPU converters exactly.
__device__ __forceinline__ void cudaEmitUYVY(unsigned char* dst, unsigned outWidth,
                                             unsigned x0, unsigned oy,
                                             const float rgb1[3], const float rgb2[3])
{
    const float y1 = 0.2126f * rgb1[0] + 0.7152f * rgb1[1] + 0.0722f * rgb1[2];
    const float y2 = 0.2126f * rgb2[0] + 0.7152f * rgb2[1] + 0.0722f * rgb2[2];
    const float avgR = (rgb1[0] + rgb2[0]) * 0.5f;
    const float avgG = (rgb1[1] + rgb2[1]) * 0.5f;
    const float avgB = (rgb1[2] + rgb2[2]) * 0.5f;
    const float u = -0.1146f * avgR - 0.3854f * avgG + 0.5f * avgB;
    const float v = 0.5f * avgR - 0.4542f * avgG - 0.0458f * avgB;

    unsigned char* out = dst + (static_cast<size_t>(oy) * outWidth + x0) * 2u;
    out[0] = static_cast<unsigned char>((u + 0.5f) * 255.0f);
    out[1] = static_cast<unsigned char>(y1 * 255.0f);
    out[2] = static_cast<unsigned char>((v + 0.5f) * 255.0f);
    out[3] = static_cast<unsigned char>(y2 * 255.0f);
}

__device__ __forceinline__ void cudaEmitP216(unsigned short* dst, unsigned outWidth, unsigned outHeight,
                                             unsigned x0, unsigned oy,
                                             const float rgb1[3], const float rgb2[3])
{
    const float y1 = 0.2627f * rgb1[0] + 0.6780f * rgb1[1] + 0.0593f * rgb1[2];
    const float y2 = 0.2627f * rgb2[0] + 0.6780f * rgb2[1] + 0.0593f * rgb2[2];
    const float avgR = (rgb1[0] + rgb2[0]) * 0.5f;
    const float avgG = (rgb1[1] + rgb2[1]) * 0.5f;
    const float avgB = (rgb1[2] + rgb2[2]) * 0.5f;
    const float u = -0.1396f * avgR - 0.3604f * avgG + 0.5f * avgB;
    const float v = 0.5f * avgR - 0.4598f * avgG - 0.0402f * avgB;

    unsigned short* yPlane = dst;
    unsigned short* uvPlane = dst + static_cast<size_t>(outWidth) * outHeight;

    const size_t yIdx = static_cast<size_t>(oy) * outWidth + x0;
    yPlane[yIdx] = static_cast<unsigned short>(4096 + y1 * 56064);
    if (x0 + 1 < outWidth) {
        yPlane[yIdx + 1] = static_cast<unsigned short>(4096 + y2 * 56064);
    }
    const size_t uvIdx = yIdx / 2;
    uvPlane[uvIdx * 2] = static_cast<unsigned short>(32768 + u * 28672);
    uvPlane[uvIdx * 2 + 1] = static_cast<unsigned short>(32768 + v * 28672);
}

__global__ void downscaleRGBAToUYVYKernel(const float* __restrict__ src, unsigned char* dst,
                                          CudaConvertParams p)
{
    const unsigned gx = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned oy = blockIdx.y * blockDim.y + threadIdx.y;
    const unsigned x0 = gx * 2u;
    if (x0 >= p.outWidth || oy >= p.outHeight) return;

    float rgb1[3], rgb2[3];
    cudaBoxSampleRGB(src, p, x0, oy, rgb1);
    if (x0 + 1 < p.outWidth) {
        cudaBoxSampleRGB(src, p, x0 + 1, oy, rgb2);
    } else {
        rgb2[0] = rgb1[0]; rgb2[1] = rgb1[1]; rgb2[2] = rgb1[2];
    }
    cudaEmitUYVY(dst, p.outWidth, x0, oy, rgb1, rgb2);
}

__global__ void downscaleRGBAToP216Kernel(const float* __restrict__ src, unsigned short* dst,
                                          CudaConvertParams p)
{
    const unsigned gx = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned oy = blockIdx.y * blockDim.y + threadIdx.y;
    const unsigned x0 = gx * 2u;
    if (x0 >= p.outWidth || oy >= p.outHeight) return;

    float rgb1[3], rgb2[3];
    cudaBoxSampleRGB(src, p, x0, oy, rgb1);
    if (x0 + 1 < p.outWidth) {
        cudaBoxSampleRGB(src, p, x0 + 1, oy, rgb2);
    } else {
        rgb2[0] = rgb1[0]; rgb2[1] = rgb1[1]; rgb2[2] = rgb1[2];
    }
    cudaEmitP216(dst, p.outWidth, p.outHeight, x0, oy, rgb1, rgb2);
}

__global__ void warpRGBAToUYVYKernel(const float* __restrict__ src, unsigned char* dst,
                                     const float* __restrict__ mapUV, CudaConvertParams p)
{
    const unsigned gx = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned oy = blockIdx.y * blockDim.y + threadIdx.y;
    const unsigned x0 = gx * 2u;
    if (x0 >= p.outWidth || oy >= p.outHeight) return;

    float rgb1[3], rgb2[3];
    cudaWarpSampleRGB(src, mapUV, p, x0, oy, rgb1);
    if (x0 + 1 < p.outWidth) {
        cudaWarpSampleRGB(src, mapUV, p, x0 + 1, oy, rgb2);
    } else {
        rgb2[0] = rgb1[0]; rgb2[1] = rgb1[1]; rgb2[2] = rgb1[2];
    }
    cudaEmitUYVY(dst, p.outWidth, x0, oy, rgb1, rgb2);
}

__global__ void warpRGBAToP216Kernel(const float* __restrict__ src, unsigned short* dst,
                                     const float* __restrict__ mapUV, CudaConvertParams p)
{
    const unsigned gx = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned oy = blockIdx.y * blockDim.y + threadIdx.y;
    const unsigned x0 = gx * 2u;
    if (x0 >= p.outWidth || oy >= p.outHeight) return;

    float rgb1[3], rgb2[3];
    cudaWarpSampleRGB(src, mapUV, p, x0, oy, rgb1);
    if (x0 + 1 < p.outWidth) {
        cudaWarpSampleRGB(src, mapUV, p, x0 + 1, oy, rgb2);
    } else {
        rgb2[0] = rgb1[0]; rgb2[1] = rgb1[1]; rgb2[2] = rgb1[2];
    }
    cudaEmitP216(dst, p.outWidth, p.outHeight, x0, oy, rgb1, rgb2);
}

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

// One staging slot of the non-blocking fast path's ring. FREE -> BUSY at
// submit (kernel writing devBuffer, then the D2H copy filling hostBuffer,
// then the consumer reading hostBuffer via the done callback) -> FREE again
// at cuda_gpu_downscale_release. hostBuffer is pinned so the copy is a real
// async DMA; devBuffer exists because kernels scatter-writing over PCIe to
// host memory would crawl.
struct CudaAsyncSlot {
    void* devBuffer = nullptr;
    void* hostBuffer = nullptr;   // pinned (cudaMallocHost)
    size_t capacity = 0;
    cudaEvent_t evStart = nullptr; // GPU timing (spec decision 6): kernel-only,
    cudaEvent_t evEnd = nullptr;   // queue wait excluded, honest under contention
    bool busy = false;
    // Completion payload, valid while busy.
    cuda_downscale_done_fn done = nullptr;
    void* user = nullptr;
    size_t outBytes = 0;
    struct CudaGPUContext* owner = nullptr;
};

#define CUDA_ASYNC_SLOTS 4

struct CudaGPUContext {
    cudaStream_t ownStream = nullptr;  // NULL-stream callers (tests, uploads)
    std::mutex asyncMutex;             // guards the slot ring's busy flags
    CudaAsyncSlot asyncSlots[CUDA_ASYNC_SLOTS];

    // Blocking-path staging (device side only; the blocking copy lands
    // straight in the caller's CPU buffer). Grown as needed; unguarded like
    // the Metal fast-path staging — callers serialize per the contract.
    void* stagingDev = nullptr;
    size_t stagingDevCap = 0;

    // Completion dispatcher: cudaLaunchHostFunc callbacks may not make CUDA
    // API calls, so they only queue the finished slot here; this thread reads
    // the timing events and invokes the done callback.
    std::mutex dispatchMutex;
    std::condition_variable dispatchCv;
    std::queue<CudaAsyncSlot*> dispatchQueue;
    bool dispatchStop = false;
    std::thread dispatcher;
};

// Sizes of every buffer this module allocated, keyed by device pointer. A raw
// CUDA device pointer has no queryable length (unlike an MTLBuffer), so this
// is what lets warp-map validation refuse an undersized upload. Module-global
// because cuda_gpu_release_buffer is called without a context (StmapEntry's
// destructor, mirroring metal_gpu_release_buffer).
static std::mutex gBufferRegistryMutex;
static std::unordered_map<void*, size_t> gBufferRegistry;

// 0 = unknown (a host-owned buffer we must trust, like every OFX CUDA plugin).
static size_t trackedBufferBytes(void* devPtr)
{
    std::lock_guard<std::mutex> lock(gBufferRegistryMutex);
    auto it = gBufferRegistry.find(devPtr);
    return it != gBufferRegistry.end() ? it->second : 0;
}

bool cuda_gpu_is_available(void)
{
    int count = 0;
    return cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
}

static void dispatcherLoop(CudaGPUContext* context)
{
    for (;;) {
        CudaAsyncSlot* slot = nullptr;
        {
            std::unique_lock<std::mutex> lock(context->dispatchMutex);
            context->dispatchCv.wait(lock, [context] {
                return context->dispatchStop || !context->dispatchQueue.empty();
            });
            if (context->dispatchStop && context->dispatchQueue.empty()) {
                return;
            }
            slot = context->dispatchQueue.front();
            context->dispatchQueue.pop();
        }
        // The host function ran, so everything enqueued before it — kernel and
        // D2H copy — has completed. A failed kernel surfaces on the event.
        const bool ok = (cudaEventQuery(slot->evEnd) == cudaSuccess);
        float gpuMs = 0.0f;
        if (ok && cudaEventElapsedTime(&gpuMs, slot->evStart, slot->evEnd) != cudaSuccess) {
            gpuMs = 0.0f;
        }
        if (!ok) {
            CUDA_LOG("Fast path async: kernel failed: %s", cudaGetErrorString(cudaEventQuery(slot->evEnd)));
        }
        // Invoked outside every module lock: done may (indirectly) call
        // cuda_gpu_downscale_release.
        slot->done(slot->user, slot, slot->hostBuffer, slot->outBytes,
                   static_cast<double>(gpuMs), ok);
    }
}

// cudaLaunchHostFunc callback: runs on CUDA's host-callback thread and blocks
// every later host function on this stream — including the host's own — so it
// only queues the slot and wakes the dispatcher. CUDA API calls are not
// permitted here.
static void CUDART_CB slotCompletedHostFn(void* userData)
{
    CudaAsyncSlot* slot = static_cast<CudaAsyncSlot*>(userData);
    CudaGPUContext* context = slot->owner;
    {
        std::lock_guard<std::mutex> lock(context->dispatchMutex);
        context->dispatchQueue.push(slot);
    }
    context->dispatchCv.notify_one();
}

CudaGPUContextRef cuda_gpu_init(void)
{
    CUDA_LOG("Initializing CUDA GPU acceleration...");

    int device = 0;
    if (cudaGetDevice(&device) != cudaSuccess) {
        CUDA_LOG("cudaGetDevice failed — no usable CUDA runtime");
        return nullptr;
    }
    cudaDeviceProp prop = {};
    if (cudaGetDeviceProperties(&prop, device) == cudaSuccess) {
        CUDA_LOG("CUDA device %d: %s (compute %d.%d)", device, prop.name, prop.major, prop.minor);
    }

    CudaGPUContext* context = new CudaGPUContext();
    if (cudaStreamCreateWithFlags(&context->ownStream, cudaStreamNonBlocking) != cudaSuccess) {
        CUDA_LOG("Failed to create module stream");
        delete context;
        return nullptr;
    }
    for (int i = 0; i < CUDA_ASYNC_SLOTS; ++i) {
        if (cudaEventCreate(&context->asyncSlots[i].evStart) != cudaSuccess ||
            cudaEventCreate(&context->asyncSlots[i].evEnd) != cudaSuccess) {
            CUDA_LOG("Failed to create timing events");
            for (int j = 0; j <= i; ++j) {
                if (context->asyncSlots[j].evStart) cudaEventDestroy(context->asyncSlots[j].evStart);
                if (context->asyncSlots[j].evEnd) cudaEventDestroy(context->asyncSlots[j].evEnd);
            }
            cudaStreamDestroy(context->ownStream);
            delete context;
            return nullptr;
        }
        context->asyncSlots[i].owner = context;
    }
    context->dispatcher = std::thread(dispatcherLoop, context);

    CUDA_LOG("CUDA GPU acceleration initialized successfully");
    return context;
}

void cuda_gpu_shutdown(CudaGPUContextRef context)
{
    if (!context) return;

    CUDA_LOG("Shutting down CUDA GPU acceleration...");

    // Async slots: the plugin drains its pump (all done callbacks fired, all
    // slots released) before shutting the context down; this wait is the
    // backstop so a straggling completion can't touch freed slots.
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        bool anyBusy = true;
        while (anyBusy && std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard<std::mutex> lock(context->asyncMutex);
                anyBusy = false;
                for (int i = 0; i < CUDA_ASYNC_SLOTS; ++i) {
                    anyBusy = anyBusy || context->asyncSlots[i].busy;
                }
            }
            if (anyBusy) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        if (anyBusy) {
            // A host function or the dispatcher may still fire and touch the
            // slot array — leak the whole context (dispatcher thread included)
            // rather than free memory a straggler will dereference.
            CUDA_LOG("Shutdown: async slot still busy after 2s — leaking the CUDA context to stay safe");
            return;
        }
    }

    {
        std::lock_guard<std::mutex> lock(context->dispatchMutex);
        context->dispatchStop = true;
    }
    context->dispatchCv.notify_all();
    if (context->dispatcher.joinable()) {
        context->dispatcher.join();
    }

    for (int i = 0; i < CUDA_ASYNC_SLOTS; ++i) {
        CudaAsyncSlot& slot = context->asyncSlots[i];
        if (slot.devBuffer) cudaFree(slot.devBuffer);
        if (slot.hostBuffer) cudaFreeHost(slot.hostBuffer);
        if (slot.evStart) cudaEventDestroy(slot.evStart);
        if (slot.evEnd) cudaEventDestroy(slot.evEnd);
    }
    if (context->stagingDev) cudaFree(context->stagingDev);
    cudaStreamDestroy(context->ownStream);
    delete context;
}

// ---------------------------------------------------------------------------
// Shared validation + launch plumbing
// ---------------------------------------------------------------------------

static cudaStream_t resolveStream(CudaGPUContextRef context, void* cudaStream)
{
    if (cudaStream) return static_cast<cudaStream_t>(cudaStream);
    return context ? context->ownStream : nullptr;
}

// A usable map buffer holds mapWidth*mapHeight interleaved (u,v) float pairs.
// Size is checkable only for module-tracked allocations (see gBufferRegistry).
static bool validWarpMap(void* mapDeviceBuffer, int mapWidth, int mapHeight)
{
    if (!mapDeviceBuffer || mapWidth <= 0 || mapHeight <= 0) return false;
    const size_t neededBytes =
        static_cast<size_t>(mapWidth) * static_cast<size_t>(mapHeight) * 2 * sizeof(float);
    const size_t tracked = trackedBufferBytes(mapDeviceBuffer);
    return tracked == 0 || tracked >= neededBytes;
}

// Geometry gate shared by every convert entry point; mirrors the Metal
// module so both platforms refuse the same sources (odd widths can't pack
// 4:2:2 rows cleanly — the caller falls back to the CPU path).
static bool validConvertGeometry(int srcWidth, int srcHeight, int srcRowFloats,
                                 int divisor, int outWidth, int outHeight)
{
    return srcWidth > 0 && srcHeight > 0 && srcRowFloats >= srcWidth * 4 &&
           divisor > 0 && outWidth >= 2 && (outWidth % 2) == 0 && outHeight > 0;
}

// Source-buffer size check, possible only when the buffer is module-tracked
// (tests); the host's frame buffers are trusted.
static bool srcBufferLargeEnough(void* srcDeviceBuffer, int srcHeight, int srcRowFloats)
{
    const size_t tracked = trackedBufferBytes(srcDeviceBuffer);
    if (tracked == 0) return true;
    const size_t neededBytes =
        static_cast<size_t>(srcHeight) * static_cast<size_t>(srcRowFloats) * sizeof(float);
    return tracked >= neededBytes;
}

static void launchConvertKernel(cudaStream_t stream, void* srcDeviceBuffer, void* dstDeviceBuffer,
                                void* mapDeviceBuffer, const CudaConvertParams& p, bool p216)
{
    const dim3 block(16, 16, 1);
    const dim3 grid(((p.outWidth / 2) + block.x - 1) / block.x,
                    (p.outHeight + block.y - 1) / block.y, 1);
    const float* src = static_cast<const float*>(srcDeviceBuffer);
    if (mapDeviceBuffer) {
        const float* mapUV = static_cast<const float*>(mapDeviceBuffer);
        if (p216) {
            warpRGBAToP216Kernel<<<grid, block, 0, stream>>>(
                src, static_cast<unsigned short*>(dstDeviceBuffer), mapUV, p);
        } else {
            warpRGBAToUYVYKernel<<<grid, block, 0, stream>>>(
                src, static_cast<unsigned char*>(dstDeviceBuffer), mapUV, p);
        }
    } else {
        if (p216) {
            downscaleRGBAToP216Kernel<<<grid, block, 0, stream>>>(
                src, static_cast<unsigned short*>(dstDeviceBuffer), p);
        } else {
            downscaleRGBAToUYVYKernel<<<grid, block, 0, stream>>>(
                src, static_cast<unsigned char*>(dstDeviceBuffer), p);
        }
    }
}

static CudaConvertParams makeParams(int srcWidth, int srcHeight, int srcRowFloats,
                                    int divisor, int outWidth, int outHeight,
                                    int mapWidth, int mapHeight)
{
    CudaConvertParams p;
    p.srcWidth = static_cast<unsigned>(srcWidth);
    p.srcHeight = static_cast<unsigned>(srcHeight);
    p.srcRowFloats = static_cast<unsigned>(srcRowFloats);
    p.outWidth = static_cast<unsigned>(outWidth);
    p.outHeight = static_cast<unsigned>(outHeight);
    p.divisor = static_cast<unsigned>(divisor);
    p.mapWidth = static_cast<unsigned>(mapWidth);
    p.mapHeight = static_cast<unsigned>(mapHeight);
    return p;
}

// Blocking convert: run the fused kernel into the context staging buffer and
// copy the small converted frame straight to cpuOut, waiting on the stream.
// Slow-path/tests only — the render fast path uses the submit entry points.
static bool runConvertKernel(CudaGPUContextRef context, void* cudaStream, void* srcDeviceBuffer,
                             int srcWidth, int srcHeight, int srcRowFloats,
                             void* mapDeviceBuffer, int mapWidth, int mapHeight,
                             int divisor,
                             int outWidth, int outHeight,
                             bool p216, void* cpuOut, size_t outBytes, const char* label)
{
    if (!context || !srcDeviceBuffer || !cpuOut) return false;
    if (!validConvertGeometry(srcWidth, srcHeight, srcRowFloats, divisor, outWidth, outHeight)) {
        return false;
    }
    const bool warp = (mapDeviceBuffer != nullptr);
    if (warp && !validWarpMap(mapDeviceBuffer, mapWidth, mapHeight)) {
        return false;
    }
    if (!srcBufferLargeEnough(srcDeviceBuffer, srcHeight, srcRowFloats)) {
        CUDA_LOG("Fast path: source buffer too small");
        return false;
    }
    cudaStream_t stream = resolveStream(context, cudaStream);
    if (!stream) return false;

    const auto startTime = std::chrono::high_resolution_clock::now();

    if (context->stagingDevCap < outBytes) {
        if (context->stagingDev) {
            cudaFree(context->stagingDev);
            context->stagingDev = nullptr;
            context->stagingDevCap = 0;
        }
        if (cudaMalloc(&context->stagingDev, outBytes) != cudaSuccess) {
            CUDA_LOG("Fast path: failed to allocate %zu-byte staging buffer", outBytes);
            context->stagingDev = nullptr;
            return false;
        }
        context->stagingDevCap = outBytes;
    }

    const CudaConvertParams p = makeParams(srcWidth, srcHeight, srcRowFloats,
                                           divisor, outWidth, outHeight, mapWidth, mapHeight);
    launchConvertKernel(stream, srcDeviceBuffer, context->stagingDev, mapDeviceBuffer, p, p216);
    cudaError_t err = cudaGetLastError();
    if (err != cudaSuccess) {
        CUDA_LOG("Fast path: kernel launch failed: %s", cudaGetErrorString(err));
        return false;
    }
    err = cudaMemcpyAsync(cpuOut, context->stagingDev, outBytes, cudaMemcpyDeviceToHost, stream);
    if (err == cudaSuccess) {
        err = cudaStreamSynchronize(stream);
    }
    if (err != cudaSuccess) {
        CUDA_LOG("Fast path: readback failed: %s", cudaGetErrorString(err));
        return false;
    }

    const auto endTime = std::chrono::high_resolution_clock::now();
    const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);
    CUDA_LOG("Fast path %s: %dx%d -> %dx%d (divisor %d) in %lld us (%.2f ms)",
             label, srcWidth, srcHeight, outWidth, outHeight, divisor,
             static_cast<long long>(duration.count()), duration.count() / 1000.0);
    return true;
}

bool cuda_gpu_buffer_downscale_to_uyvy(CudaGPUContextRef context,
                                       void* cudaStream,
                                       void* srcDeviceBuffer,
                                       int srcWidth, int srcHeight, int srcRowFloats,
                                       int divisor,
                                       int outWidth, int outHeight,
                                       unsigned char* uyvyOut)
{
    const size_t outBytes = static_cast<size_t>(outWidth) * static_cast<size_t>(outHeight) * 2;
    return runConvertKernel(context, cudaStream, srcDeviceBuffer,
                            srcWidth, srcHeight, srcRowFloats, nullptr, 0, 0, divisor,
                            outWidth, outHeight, false, uyvyOut, outBytes, "UYVY");
}

bool cuda_gpu_buffer_downscale_to_p216(CudaGPUContextRef context,
                                       void* cudaStream,
                                       void* srcDeviceBuffer,
                                       int srcWidth, int srcHeight, int srcRowFloats,
                                       int divisor,
                                       int outWidth, int outHeight,
                                       unsigned short* p216Out)
{
    const size_t outBytes = static_cast<size_t>(outWidth) * static_cast<size_t>(outHeight) * 2 * sizeof(unsigned short);
    return runConvertKernel(context, cudaStream, srcDeviceBuffer,
                            srcWidth, srcHeight, srcRowFloats, nullptr, 0, 0, divisor,
                            outWidth, outHeight, true, p216Out, outBytes, "P216");
}

bool cuda_gpu_buffer_warp_to_uyvy(CudaGPUContextRef context,
                                  void* cudaStream,
                                  void* srcDeviceBuffer,
                                  int srcWidth, int srcHeight, int srcRowFloats,
                                  void* mapDeviceBuffer, int mapWidth, int mapHeight,
                                  int divisor,
                                  int outWidth, int outHeight,
                                  unsigned char* uyvyOut)
{
    if (!mapDeviceBuffer) return false;
    const size_t outBytes = static_cast<size_t>(outWidth) * static_cast<size_t>(outHeight) * 2;
    return runConvertKernel(context, cudaStream, srcDeviceBuffer,
                            srcWidth, srcHeight, srcRowFloats,
                            mapDeviceBuffer, mapWidth, mapHeight, divisor,
                            outWidth, outHeight, false, uyvyOut, outBytes, "warp UYVY");
}

bool cuda_gpu_buffer_warp_to_p216(CudaGPUContextRef context,
                                  void* cudaStream,
                                  void* srcDeviceBuffer,
                                  int srcWidth, int srcHeight, int srcRowFloats,
                                  void* mapDeviceBuffer, int mapWidth, int mapHeight,
                                  int divisor,
                                  int outWidth, int outHeight,
                                  unsigned short* p216Out)
{
    if (!mapDeviceBuffer) return false;
    const size_t outBytes = static_cast<size_t>(outWidth) * static_cast<size_t>(outHeight) * 2 * sizeof(unsigned short);
    return runConvertKernel(context, cudaStream, srcDeviceBuffer,
                            srcWidth, srcHeight, srcRowFloats,
                            mapDeviceBuffer, mapWidth, mapHeight, divisor,
                            outWidth, outHeight, true, p216Out, outBytes, "warp P216");
}

// Non-blocking variant: enqueue only. Validation mirrors runConvertKernel so
// both paths refuse the same sources; refusals are typed (BUSY vs INVALID)
// because the caller's correct reaction differs — drop vs fall back to the
// blocking readback. mapDeviceBuffer selects the STMap warp kernels; null =
// plain downscale.
static cuda_submit_status submitConvertInternal(CudaGPUContextRef context,
                                                void* cudaStream,
                                                void* srcDeviceBuffer,
                                                int srcWidth, int srcHeight, int srcRowFloats,
                                                void* mapDeviceBuffer, int mapWidth, int mapHeight,
                                                int divisor,
                                                int outWidth, int outHeight,
                                                bool p216,
                                                cuda_downscale_done_fn done, void* user)
{
    if (!context || !srcDeviceBuffer || !done) return CUDA_SUBMIT_INVALID;
    if (!validConvertGeometry(srcWidth, srcHeight, srcRowFloats, divisor, outWidth, outHeight)) {
        return CUDA_SUBMIT_INVALID;
    }
    const bool warp = (mapDeviceBuffer != nullptr);
    if (warp && !validWarpMap(mapDeviceBuffer, mapWidth, mapHeight)) {
        return CUDA_SUBMIT_INVALID;
    }
    if (!srcBufferLargeEnough(srcDeviceBuffer, srcHeight, srcRowFloats)) {
        return CUDA_SUBMIT_INVALID;
    }
    cudaStream_t stream = resolveStream(context, cudaStream);
    if (!stream) return CUDA_SUBMIT_INVALID;
    const size_t outBytes = static_cast<size_t>(outWidth) * outHeight * 2 *
                            (p216 ? sizeof(unsigned short) : 1);

    // Claim a free slot; none free = GPU behind or consumer backlogged — the
    // caller drops this frame (backpressure by dropping, never blocking).
    CudaAsyncSlot* slot = nullptr;
    {
        std::lock_guard<std::mutex> lock(context->asyncMutex);
        for (int i = 0; i < CUDA_ASYNC_SLOTS; ++i) {
            if (!context->asyncSlots[i].busy) {
                slot = &context->asyncSlots[i];
                slot->busy = true;
                break;
            }
        }
    }
    if (!slot) return CUDA_SUBMIT_BUSY;

    if (slot->capacity < outBytes) {
        if (slot->devBuffer) cudaFree(slot->devBuffer);
        if (slot->hostBuffer) cudaFreeHost(slot->hostBuffer);
        slot->devBuffer = nullptr;
        slot->hostBuffer = nullptr;
        slot->capacity = 0;
        if (cudaMalloc(&slot->devBuffer, outBytes) != cudaSuccess ||
            cudaMallocHost(&slot->hostBuffer, outBytes) != cudaSuccess) {
            if (slot->devBuffer) cudaFree(slot->devBuffer);
            slot->devBuffer = nullptr;
            std::lock_guard<std::mutex> lock(context->asyncMutex);
            slot->busy = false;
            return CUDA_SUBMIT_INVALID; // allocation failure won't heal frame-to-frame
        }
        slot->capacity = outBytes;
    }

    slot->done = done;
    slot->user = user;
    slot->outBytes = outBytes;

    const CudaConvertParams p = makeParams(srcWidth, srcHeight, srcRowFloats,
                                           divisor, outWidth, outHeight, mapWidth, mapHeight);
    cudaEventRecord(slot->evStart, stream);
    launchConvertKernel(stream, srcDeviceBuffer, slot->devBuffer, mapDeviceBuffer, p, p216);
    cudaError_t err = cudaGetLastError();
    if (err == cudaSuccess) err = cudaEventRecord(slot->evEnd, stream);
    if (err == cudaSuccess) {
        err = cudaMemcpyAsync(slot->hostBuffer, slot->devBuffer, outBytes,
                              cudaMemcpyDeviceToHost, stream);
    }
    if (err == cudaSuccess) err = cudaLaunchHostFunc(stream, slotCompletedHostFn, slot);
    if (err != cudaSuccess) {
        // Enqueue failed partway: no completion will fire. Practically this
        // means a broken context (launches only fail synchronously for
        // configuration errors); free the slot so the stream can fall back.
        CUDA_LOG("Fast path async: enqueue failed: %s", cudaGetErrorString(err));
        std::lock_guard<std::mutex> lock(context->asyncMutex);
        slot->busy = false;
        return CUDA_SUBMIT_INVALID;
    }
    return CUDA_SUBMIT_OK;
}

cuda_submit_status cuda_gpu_downscale_submit(CudaGPUContextRef context,
                                             void* cudaStream,
                                             void* srcDeviceBuffer,
                                             int srcWidth, int srcHeight, int srcRowFloats,
                                             int divisor,
                                             int outWidth, int outHeight,
                                             bool p216,
                                             cuda_downscale_done_fn done, void* user)
{
    return submitConvertInternal(context, cudaStream, srcDeviceBuffer,
                                 srcWidth, srcHeight, srcRowFloats,
                                 nullptr, 0, 0, divisor,
                                 outWidth, outHeight, p216, done, user);
}

cuda_submit_status cuda_gpu_warp_submit(CudaGPUContextRef context,
                                        void* cudaStream,
                                        void* srcDeviceBuffer,
                                        int srcWidth, int srcHeight, int srcRowFloats,
                                        void* mapDeviceBuffer, int mapWidth, int mapHeight,
                                        int divisor,
                                        int outWidth, int outHeight,
                                        bool p216,
                                        cuda_downscale_done_fn done, void* user)
{
    if (!mapDeviceBuffer) return CUDA_SUBMIT_INVALID;
    return submitConvertInternal(context, cudaStream, srcDeviceBuffer,
                                 srcWidth, srcHeight, srcRowFloats,
                                 mapDeviceBuffer, mapWidth, mapHeight, divisor,
                                 outWidth, outHeight, p216, done, user);
}

void cuda_gpu_downscale_release(CudaGPUContextRef context, void* slot)
{
    if (!context || !slot) return;
    std::lock_guard<std::mutex> lock(context->asyncMutex);
    static_cast<CudaAsyncSlot*>(slot)->busy = false;
}

bool cuda_gpu_copy_buffer(CudaGPUContextRef context,
                          void* cudaStream,
                          void* srcDeviceBuffer, void* dstDeviceBuffer,
                          size_t byteCount, bool waitForCompletion)
{
    if (!srcDeviceBuffer || !dstDeviceBuffer || byteCount == 0) return false;
    cudaStream_t stream = resolveStream(context, cudaStream);
    if (!stream) return false;

    size_t copyBytes = byteCount;
    const size_t srcTracked = trackedBufferBytes(srcDeviceBuffer);
    const size_t dstTracked = trackedBufferBytes(dstDeviceBuffer);
    if (srcTracked && copyBytes > srcTracked) copyBytes = srcTracked;
    if (dstTracked && copyBytes > dstTracked) copyBytes = dstTracked;
    if (copyBytes != byteCount) {
        CUDA_LOG("Passthrough copy clamped from %zu to %zu bytes", byteCount, copyBytes);
    }

    cudaError_t err = cudaMemcpyAsync(dstDeviceBuffer, srcDeviceBuffer, copyBytes,
                                      cudaMemcpyDeviceToDevice, stream);
    if (err != cudaSuccess) {
        CUDA_LOG("Passthrough copy failed: %s", cudaGetErrorString(err));
        return false;
    }
    if (waitForCompletion) {
        return cudaStreamSynchronize(stream) == cudaSuccess;
    }
    return true;
}

bool cuda_gpu_read_buffer(CudaGPUContextRef context,
                          void* cudaStream,
                          void* srcDeviceBuffer,
                          void* cpuDst, size_t byteCount)
{
    if (!srcDeviceBuffer || !cpuDst || byteCount == 0) return false;
    const size_t tracked = trackedBufferBytes(srcDeviceBuffer);
    if (tracked && byteCount > tracked) return false;
    cudaStream_t stream = resolveStream(context, cudaStream);
    if (!stream) return false;

    cudaError_t err = cudaMemcpyAsync(cpuDst, srcDeviceBuffer, byteCount,
                                      cudaMemcpyDeviceToHost, stream);
    if (err == cudaSuccess) {
        err = cudaStreamSynchronize(stream);
    }
    return err == cudaSuccess;
}

// Upload synchronizes before returning so callers may immediately hand the
// pointer to kernels on ANY stream — the caller's stream never sees a
// half-uploaded map (Metal's newBufferWithBytes has the same semantics via
// its CPU-side copy).
static void* createTrackedBuffer(cudaStream_t stream, const void* initialData, size_t byteCount)
{
    if (!stream || byteCount == 0) return nullptr;
    void* devPtr = nullptr;
    if (cudaMalloc(&devPtr, byteCount) != cudaSuccess) {
        return nullptr;
    }
    if (initialData) {
        cudaError_t err = cudaMemcpyAsync(devPtr, initialData, byteCount,
                                          cudaMemcpyHostToDevice, stream);
        if (err == cudaSuccess) err = cudaStreamSynchronize(stream);
        if (err != cudaSuccess) {
            cudaFree(devPtr);
            return nullptr;
        }
    }
    std::lock_guard<std::mutex> lock(gBufferRegistryMutex);
    gBufferRegistry[devPtr] = byteCount;
    return devPtr;
}

void* cuda_gpu_create_device_buffer(CudaGPUContextRef context, const void* initialData, size_t byteCount)
{
    if (!context) return nullptr;
    return createTrackedBuffer(context->ownStream, initialData, byteCount);
}

void* cuda_gpu_create_device_buffer_for_stream(CudaGPUContextRef context, void* cudaStream,
                                               const void* initialData, size_t byteCount)
{
    return createTrackedBuffer(resolveStream(context, cudaStream), initialData, byteCount);
}

void* cuda_gpu_stream_device(CudaGPUContextRef context, void* cudaStream)
{
    (void)context;
    (void)cudaStream;
    // CUDA device memory is process-wide under unified addressing; key caches
    // by the calling thread's current device (+1 so device 0 isn't NULL).
    int device = 0;
    if (cudaGetDevice(&device) != cudaSuccess) return nullptr;
    return reinterpret_cast<void*>(static_cast<intptr_t>(device) + 1);
}

void cuda_gpu_release_buffer(void* deviceBuffer)
{
    if (!deviceBuffer) return;
    {
        std::lock_guard<std::mutex> lock(gBufferRegistryMutex);
        gBufferRegistry.erase(deviceBuffer);
    }
    // cudaFree (unlike cudaFreeAsync) does not release memory out from under
    // still-running work that references it — the driver orders the free
    // behind prior launches — so an in-flight frame on a dying STMap entry is
    // safe, mirroring Metal's retain-while-executing semantics.
    cudaFree(deviceBuffer);
}

#endif // _WIN32
