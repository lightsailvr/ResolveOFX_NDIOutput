#ifndef METAL_GPU_ACCELERATION_H
#define METAL_GPU_ACCELERATION_H

#ifdef __APPLE__

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle for Metal GPU context
typedef struct MetalGPUContext* MetalGPUContextRef;

// Initialize Metal GPU context
MetalGPUContextRef metal_gpu_init(void);

// Shutdown Metal GPU context
void metal_gpu_shutdown(MetalGPUContextRef context);

// Convert RGBA float to UYVY using Metal compute shader
bool metal_gpu_convert_rgba_to_uyvy(MetalGPUContextRef context, 
                                   const float* rgbaData, 
                                   unsigned char* uyvyData,
                                   int width, 
                                   int height);

// Convert RGBA float to 16-bit RGBA for HDR using Metal
bool metal_gpu_convert_rgba_to_hdr(MetalGPUContextRef context,
                                  const float* rgbaData,
                                  unsigned short* hdrData,
                                  int width,
                                  int height,
                                  float scale);

// ---------------------------------------------------------------------------
// GPU-native fast path (issue #5): the host hands the frame as an id<MTLBuffer>
// of tightly-packed float RGBA. These fuse box-downscale (divisor 1/2/4) +
// vertical flip + color conversion on the GPU and read back only the small
// converted frame. srcMetalBuffer must belong to the device of commandQueue
// (pass the host's queue so the work is ordered after the host's own renders);
// commandQueue == NULL uses a context-owned queue on the default device (tests).
// Output dimensions come from ndi_stream::outputDims(). Not thread-safe per
// context — callers serialize (the plugin holds its GPUContext mutex).
// ---------------------------------------------------------------------------

// Fused downscale + RGBA float → UYVY. uyvyOut receives outWidth*outHeight*2 bytes.
bool metal_gpu_buffer_downscale_to_uyvy(MetalGPUContextRef context,
                                        void* commandQueue,
                                        void* srcMetalBuffer,
                                        int srcWidth, int srcHeight, int srcRowFloats,
                                        int divisor,
                                        int outWidth, int outHeight,
                                        unsigned char* uyvyOut);

// Fused downscale + RGBA float → P216 (planar Y then interleaved UV, 16-bit,
// BT.2100 limited range). p216Out receives outWidth*outHeight*2 uint16 values.
bool metal_gpu_buffer_downscale_to_p216(MetalGPUContextRef context,
                                        void* commandQueue,
                                        void* srcMetalBuffer,
                                        int srcWidth, int srcHeight, int srcRowFloats,
                                        int divisor,
                                        int outWidth, int outHeight,
                                        unsigned short* p216Out);

// ---------------------------------------------------------------------------
// STMap warp variants (issue #7): same fused structure, but each output pixel
// gathers through an STMap instead of a fixed box grid — the map texel for a
// full-res destination pixel gives the normalized source position to sample
// bilinearly, and divisor 2/4 box-averages those full-res warped taps. Bit-for-
// bit the composition ndi_stmap::warpRGBABox + the flipping converters (the
// identity checked by make test-metal). mapMetalBuffer holds mapWidth*mapHeight
// interleaved (u,v) float pairs, row 0 = top (STMapImage::uv verbatim), on the
// same device as commandQueue. Output dimensions come from
// ndi_stream::outputDims(mapWidth, mapHeight, divisor) — the map defines the
// destination image.
// ---------------------------------------------------------------------------

bool metal_gpu_buffer_warp_to_uyvy(MetalGPUContextRef context,
                                   void* commandQueue,
                                   void* srcMetalBuffer,
                                   int srcWidth, int srcHeight, int srcRowFloats,
                                   void* mapMetalBuffer, int mapWidth, int mapHeight,
                                   int divisor,
                                   int outWidth, int outHeight,
                                   unsigned char* uyvyOut);

bool metal_gpu_buffer_warp_to_p216(MetalGPUContextRef context,
                                   void* commandQueue,
                                   void* srcMetalBuffer,
                                   int srcWidth, int srcHeight, int srcRowFloats,
                                   void* mapMetalBuffer, int mapWidth, int mapHeight,
                                   int divisor,
                                   int outWidth, int outHeight,
                                   unsigned short* p216Out);

// ---------------------------------------------------------------------------
// Non-blocking fast path (issue #5, v1.6.0): same fused kernels, but the
// render thread only ENCODES — no waitUntilCompleted. The kernel writes into
// one of a small ring of context-owned shared-storage staging buffers
// (CPU-visible on Apple Silicon: the small converted frame needs no separate
// readback), and `done` fires from the command buffer's completion handler.
//
//   done(user, slot, outPtr, outBytes, ok)
//
// Contract:
// - `done` runs on Metal's completion thread. It must be near-free (push a
//   pointer, wake a worker) — heavy work there stalls the queue's other
//   completion handlers, including the host's own.
// - outPtr stays valid until metal_gpu_downscale_release(context, slot). The
//   consumer copies or fully consumes it, then releases; a slot is never
//   reused while unreleased.
// - Returns false (and never calls done) when no slot is free — GPU behind or
//   consumer backlogged. Callers drop the frame; dropping is the backpressure.
// - ok=false reports a failed command buffer; release the slot regardless.
// ---------------------------------------------------------------------------
// gpuMs is the kernel's GPU execution time (GPUEndTime-GPUStartTime) — queue
// wait excluded, so it stays honest under host contention.
typedef void (*metal_downscale_done_fn)(void* user, void* slot,
                                        const void* outPtr, size_t outBytes,
                                        double gpuMs, bool ok);

// Why a submit was refused — the caller's reaction differs:
// BUSY   = every slot in flight (transient): drop this frame, try next render.
// INVALID = geometry/pipeline/allocation failure (persistent for this source):
//           fall back to the blocking readback path so the stream survives.
typedef enum {
    METAL_SUBMIT_OK = 0,
    METAL_SUBMIT_BUSY = 1,
    METAL_SUBMIT_INVALID = 2,
} metal_submit_status;

metal_submit_status metal_gpu_downscale_submit(MetalGPUContextRef context,
                                               void* commandQueue,
                                               void* srcMetalBuffer,
                                               int srcWidth, int srcHeight, int srcRowFloats,
                                               int divisor,
                                               int outWidth, int outHeight,
                                               bool p216,
                                               metal_downscale_done_fn done, void* user);

// Non-blocking STMap warp: identical contract to metal_gpu_downscale_submit
// (same slot ring, same done callback, same BUSY/INVALID semantics), with the
// warp kernels instead of the plain downscale. A null/undersized map buffer is
// INVALID — the caller falls back to the CPU warp so the stream survives.
metal_submit_status metal_gpu_warp_submit(MetalGPUContextRef context,
                                          void* commandQueue,
                                          void* srcMetalBuffer,
                                          int srcWidth, int srcHeight, int srcRowFloats,
                                          void* mapMetalBuffer, int mapWidth, int mapHeight,
                                          int divisor,
                                          int outWidth, int outHeight,
                                          bool p216,
                                          metal_downscale_done_fn done, void* user);

void metal_gpu_downscale_release(MetalGPUContextRef context, void* slot);

// Device-to-device copy of byteCount bytes (the render passthrough when the
// host hands Metal buffers). Committed to commandQueue; waits for completion
// only when waitForCompletion is set (tests) — the host orders its own
// downstream reads on the same queue.
bool metal_gpu_copy_buffer(MetalGPUContextRef context,
                           void* commandQueue,
                           void* srcMetalBuffer, void* dstMetalBuffer,
                           size_t byteCount, bool waitForCompletion);

// Read byteCount bytes of a device buffer back to CPU memory, ordered after
// all work already committed to commandQueue. This is the full-frame fallback
// when the GPU convert path is unavailable — slow by design, never fast-path.
bool metal_gpu_read_buffer(MetalGPUContextRef context,
                           void* commandQueue,
                           void* srcMetalBuffer,
                           void* cpuDst, size_t byteCount);

// Test helpers (make test-metal): create/inspect/release a shared-storage
// buffer on the context's device. create copies initialData when non-NULL.
void* metal_gpu_create_shared_buffer(MetalGPUContextRef context, const void* initialData, size_t byteCount);
void* metal_gpu_buffer_contents(void* metalBuffer);
void  metal_gpu_release_buffer(void* metalBuffer);

// STMap upload for the render path: like metal_gpu_create_shared_buffer but on
// commandQueue's device (which can differ from the context default on
// multi-GPU Macs — pass the host's queue). NULL commandQueue uses the context
// device. metal_gpu_queue_device returns the device pointer for cache keying.
void* metal_gpu_create_shared_buffer_for_queue(MetalGPUContextRef context, void* commandQueue,
                                               const void* initialData, size_t byteCount);
void* metal_gpu_queue_device(MetalGPUContextRef context, void* commandQueue);

// Check if Metal is available on this system
bool metal_gpu_is_available(void);

#ifdef __cplusplus
}
#endif

#endif // __APPLE__

#endif // METAL_GPU_ACCELERATION_H 