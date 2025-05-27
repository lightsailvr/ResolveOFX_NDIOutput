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

// Check if Metal is available on this system
bool metal_gpu_is_available(void);

#ifdef __cplusplus
}
#endif

#endif // __APPLE__

#endif // METAL_GPU_ACCELERATION_H 