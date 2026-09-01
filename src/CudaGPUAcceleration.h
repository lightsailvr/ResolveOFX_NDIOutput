#ifndef CUDA_GPU_ACCELERATION_H
#define CUDA_GPU_ACCELERATION_H

#ifdef _WIN32

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// CUDA implementation of the GPU-module contract (Windows port ticket #22;
// spec decision 5) — the same header-level C API the Metal module implements
// (src/MetalGPUAcceleration.h). Everything above this seam (pairing, packing,
// the async pump, policy) is platform-neutral and stays shared.
//
// Naming map from the Metal header: commandQueue -> cudaStream (the host's
// cudaStream_t, passed as void*), id<MTLBuffer> -> CUDA device pointer.
// Unlike an MTLBuffer, a raw device pointer carries no queryable length, so
// size validation only applies to buffers this module allocated itself
// (cuda_gpu_create_device_buffer*), which are tracked internally; the host's
// frame buffers are trusted, as they are on every OFX CUDA plugin.
//
// Host discipline (spec decision 6): all work enqueues on the host-provided
// CUDA stream; the render action never blocks on GPU completion in the fast
// path; never cudaDeviceSynchronize; never cudaDeviceReset. Completions fire
// via cudaLaunchHostFunc on the host's stream; the host function only queues
// the finished slot to a module-owned dispatcher thread, which computes the
// kernel's GPU time from CUDA events (an API call host functions may not
// make) and invokes the done callback. Staging slots are pinned host memory,
// filled by a device-to-host copy enqueued behind the kernel.
// ---------------------------------------------------------------------------

// Opaque handle for the CUDA GPU context
typedef struct CudaGPUContext* CudaGPUContextRef;

// Initialize / shut down the CUDA GPU context. init returns NULL when CUDA
// is unusable (no device, no driver) — callers fall back to the CPU path.
CudaGPUContextRef cuda_gpu_init(void);
void cuda_gpu_shutdown(CudaGPUContextRef context);

// True when a CUDA device is present and the runtime is usable.
bool cuda_gpu_is_available(void);

// ---------------------------------------------------------------------------
// Blocking fused kernels: box-downscale (divisor 1/2/4) + vertical flip +
// color conversion, reading the float RGBA frame the host left on the device
// and writing the small converted frame to CPU memory. Byte-identical to the
// CPU composition ndi_stream::downscaleRGBABox + the flipping CPU converters
// (tests/test_cuda_downscale.cpp; the module compiles with --fmad=false so
// GPU arithmetic matches the CPU's operation-for-operation). cudaStream ==
// NULL uses a context-owned stream (tests). Output dimensions come from
// ndi_stream::outputDims(). Not thread-safe per context — callers serialize
// (the plugin holds its GPUContext mutex).
// ---------------------------------------------------------------------------

// Fused downscale + RGBA float → UYVY. uyvyOut receives outWidth*outHeight*2 bytes.
bool cuda_gpu_buffer_downscale_to_uyvy(CudaGPUContextRef context,
                                       void* cudaStream,
                                       void* srcDeviceBuffer,
                                       int srcWidth, int srcHeight, int srcRowFloats,
                                       int divisor,
                                       int outWidth, int outHeight,
                                       unsigned char* uyvyOut);

// Fused downscale + RGBA float → P216 (planar Y then interleaved UV, 16-bit,
// BT.2100 limited range). p216Out receives outWidth*outHeight*2 uint16 values.
bool cuda_gpu_buffer_downscale_to_p216(CudaGPUContextRef context,
                                       void* cudaStream,
                                       void* srcDeviceBuffer,
                                       int srcWidth, int srcHeight, int srcRowFloats,
                                       int divisor,
                                       int outWidth, int outHeight,
                                       unsigned short* p216Out);

// ---------------------------------------------------------------------------
// STMap warp variants: same fused structure, but each output pixel gathers
// through an STMap — the map texel for a full-res destination pixel gives the
// normalized source position to sample bilinearly, and divisor 2/4
// box-averages those full-res warped taps. Byte-identical to the composition
// ndi_stmap::warpRGBABox + the flipping converters, and an identity map
// reproduces the plain downscale kernels exactly. mapDeviceBuffer holds
// mapWidth*mapHeight interleaved (u,v) float pairs, row 0 = top
// (STMapImage::uv verbatim). Output dimensions come from
// ndi_stream::outputDims(mapWidth, mapHeight, divisor) — the map defines the
// destination image.
// ---------------------------------------------------------------------------

bool cuda_gpu_buffer_warp_to_uyvy(CudaGPUContextRef context,
                                  void* cudaStream,
                                  void* srcDeviceBuffer,
                                  int srcWidth, int srcHeight, int srcRowFloats,
                                  void* mapDeviceBuffer, int mapWidth, int mapHeight,
                                  int divisor,
                                  int outWidth, int outHeight,
                                  unsigned char* uyvyOut);

bool cuda_gpu_buffer_warp_to_p216(CudaGPUContextRef context,
                                  void* cudaStream,
                                  void* srcDeviceBuffer,
                                  int srcWidth, int srcHeight, int srcRowFloats,
                                  void* mapDeviceBuffer, int mapWidth, int mapHeight,
                                  int divisor,
                                  int outWidth, int outHeight,
                                  unsigned short* p216Out);

// ---------------------------------------------------------------------------
// Non-blocking fast path: same fused kernels, but the render thread only
// ENQUEUES — kernel, device-to-pinned-host copy, and completion callback all
// stack up on the host's stream and the call returns immediately. `done`
// fires from the module's dispatcher thread once the copy lands:
//
//   done(user, slot, outPtr, outBytes, gpuMs, ok)
//
// Contract (identical to the Metal module's):
// - `done` must be near-free (push a pointer, wake a worker) — heavy work
//   there delays every later completion on the same dispatcher.
// - outPtr (pinned host memory) stays valid until
//   cuda_gpu_downscale_release(context, slot). The consumer copies or fully
//   consumes it, then releases; a slot is never reused while unreleased.
// - Returns CUDA_SUBMIT_BUSY (and never calls done) when no slot is free —
//   GPU behind or consumer backlogged. Callers drop the frame; dropping is
//   the backpressure.
// - ok=false reports a failed kernel or copy; release the slot regardless.
// - gpuMs is the kernel's GPU execution time from CUDA events — queue wait
//   excluded, so it stays honest under host contention.
// ---------------------------------------------------------------------------
typedef void (*cuda_downscale_done_fn)(void* user, void* slot,
                                       const void* outPtr, size_t outBytes,
                                       double gpuMs, bool ok);

// Why a submit was refused — the caller's reaction differs:
// BUSY   = every slot in flight (transient): drop this frame, try next render.
// INVALID = geometry/allocation failure (persistent for this source):
//           fall back to the blocking readback path so the stream survives.
typedef enum {
    CUDA_SUBMIT_OK = 0,
    CUDA_SUBMIT_BUSY = 1,
    CUDA_SUBMIT_INVALID = 2,
} cuda_submit_status;

cuda_submit_status cuda_gpu_downscale_submit(CudaGPUContextRef context,
                                             void* cudaStream,
                                             void* srcDeviceBuffer,
                                             int srcWidth, int srcHeight, int srcRowFloats,
                                             int divisor,
                                             int outWidth, int outHeight,
                                             bool p216,
                                             cuda_downscale_done_fn done, void* user);

// Non-blocking STMap warp: identical contract to cuda_gpu_downscale_submit
// (same slot ring, same done callback, same BUSY/INVALID semantics), with the
// warp kernels instead of the plain downscale. A null/undersized map buffer
// is INVALID — the caller falls back to the CPU warp so the stream survives.
cuda_submit_status cuda_gpu_warp_submit(CudaGPUContextRef context,
                                        void* cudaStream,
                                        void* srcDeviceBuffer,
                                        int srcWidth, int srcHeight, int srcRowFloats,
                                        void* mapDeviceBuffer, int mapWidth, int mapHeight,
                                        int divisor,
                                        int outWidth, int outHeight,
                                        bool p216,
                                        cuda_downscale_done_fn done, void* user);

void cuda_gpu_downscale_release(CudaGPUContextRef context, void* slot);

// Device-to-device copy of byteCount bytes (the render passthrough when the
// host hands CUDA buffers). Enqueued on cudaStream; waits for completion
// only when waitForCompletion is set (tests) — the host orders its own
// downstream reads on the same stream.
bool cuda_gpu_copy_buffer(CudaGPUContextRef context,
                          void* cudaStream,
                          void* srcDeviceBuffer, void* dstDeviceBuffer,
                          size_t byteCount, bool waitForCompletion);

// Read byteCount bytes of a device buffer back to CPU memory, ordered after
// all work already enqueued on cudaStream. This is the full-frame fallback
// when the GPU convert path is unavailable — slow by design, never fast-path.
bool cuda_gpu_read_buffer(CudaGPUContextRef context,
                          void* cudaStream,
                          void* srcDeviceBuffer,
                          void* cpuDst, size_t byteCount);

// Device-buffer helpers (tests + the STMap upload cache): allocate byteCount
// bytes of device memory, copying initialData up when non-NULL. The
// allocation is tracked internally so warp-map size validation works; release
// with cuda_gpu_release_buffer. The _for_stream variant mirrors the Metal
// _for_queue API; CUDA device memory is not queue-bound, so both allocate on
// the calling thread's current device. cuda_gpu_stream_device returns that
// device as an opaque key for per-device cache maps.
void* cuda_gpu_create_device_buffer(CudaGPUContextRef context, const void* initialData, size_t byteCount);
void* cuda_gpu_create_device_buffer_for_stream(CudaGPUContextRef context, void* cudaStream,
                                               const void* initialData, size_t byteCount);
void* cuda_gpu_stream_device(CudaGPUContextRef context, void* cudaStream);
void  cuda_gpu_release_buffer(void* deviceBuffer);

#ifdef __cplusplus
}
#endif

#endif // _WIN32

#endif // CUDA_GPU_ACCELERATION_H
