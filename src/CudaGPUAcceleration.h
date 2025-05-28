#ifndef CUDA_GPU_ACCELERATION_H
#define CUDA_GPU_ACCELERATION_H

#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32) || defined(_WIN64) || defined(__WIN64__) || defined(WIN64)

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle for CUDA GPU context
typedef struct CudaGPUContext* CudaGPUContextRef;

// Initialize CUDA GPU context
CudaGPUContextRef cuda_gpu_init(void);

// Shutdown CUDA GPU context
void cuda_gpu_shutdown(CudaGPUContextRef context);

// Convert RGBA float to UYVY using CUDA kernel
bool cuda_gpu_convert_rgba_to_uyvy(CudaGPUContextRef context, 
                                   const float* rgbaData, 
                                   unsigned char* uyvyData,
                                   int width, 
                                   int height);

// Convert RGBA float to 16-bit RGBA for HDR using CUDA
bool cuda_gpu_convert_rgba_to_hdr(CudaGPUContextRef context,
                                  const float* rgbaData,
                                  unsigned short* hdrData,
                                  int width,
                                  int height,
                                  float scale);

// Check if CUDA is available on this system
bool cuda_gpu_is_available(void);

// Get CUDA device information
const char* cuda_gpu_get_device_name(CudaGPUContextRef context);

// Get CUDA memory information
bool cuda_gpu_get_memory_info(CudaGPUContextRef context, size_t* free_mem, size_t* total_mem);

#ifdef __cplusplus
}
#endif

#endif // Windows

#endif // CUDA_GPU_ACCELERATION_H 