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

// Check if Metal is available on this system
bool metal_gpu_is_available(void);

#ifdef __cplusplus
}
#endif

#endif // __APPLE__

#endif // METAL_GPU_ACCELERATION_H 