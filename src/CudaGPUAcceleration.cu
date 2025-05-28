#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32) || defined(_WIN64) || defined(__WIN64__) || defined(WIN64)

#include "CudaGPUAcceleration.h"
#include <cuda_runtime.h>
#include <device_launch_parameters.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <chrono>

#define CUDA_LOG(fmt, ...) printf("NDI Plugin CUDA: " fmt "\n", ##__VA_ARGS__)

// CUDA error checking macro
#define CUDA_CHECK(call) \
    do { \
        cudaError_t error = call; \
        if (error != cudaSuccess) { \
            CUDA_LOG("CUDA error at %s:%d - %s", __FILE__, __LINE__, cudaGetErrorString(error)); \
            return false; \
        } \
    } while(0)

#define CUDA_CHECK_VOID(call) \
    do { \
        cudaError_t error = call; \
        if (error != cudaSuccess) { \
            CUDA_LOG("CUDA error at %s:%d - %s", __FILE__, __LINE__, cudaGetErrorString(error)); \
        } \
    } while(0)

// CUDA GPU Context structure
struct CudaGPUContext {
    int deviceId;
    cudaDeviceProp deviceProps;
    cudaStream_t stream;
    char deviceName[256];
    
    // Memory pools for better performance
    float* d_rgbaInput;
    unsigned char* d_uyvyOutput;
    unsigned short* d_hdrOutput;
    size_t allocatedInputSize;
    size_t allocatedUyvySize;
    size_t allocatedHdrSize;
};

// CUDA kernel for RGBA to UYVY conversion
__global__ void rgba_to_uyvy_kernel(
    const float4* __restrict__ rgbaInput,
    uchar4* __restrict__ uyvyOutput,
    int width,
    int height
) {
    int x = (blockIdx.x * blockDim.x + threadIdx.x) * 2; // Process two pixels at a time
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    
    if (x >= width || y >= height) return;
    
    // Flip vertically: OpenFX uses bottom-left origin, NDI expects top-left
    int srcRow = height - 1 - y;
    
    // Get two adjacent pixels
    int srcIdx1 = srcRow * width + x;
    int srcIdx2 = srcRow * width + x + 1;
    
    float4 pixel1 = rgbaInput[srcIdx1];
    float4 pixel2 = (x + 1 < width) ? rgbaInput[srcIdx2] : pixel1;
    
    // Clamp values to [0, 1]
    pixel1.x = fmaxf(0.0f, fminf(1.0f, pixel1.x));
    pixel1.y = fmaxf(0.0f, fminf(1.0f, pixel1.y));
    pixel1.z = fmaxf(0.0f, fminf(1.0f, pixel1.z));
    
    pixel2.x = fmaxf(0.0f, fminf(1.0f, pixel2.x));
    pixel2.y = fmaxf(0.0f, fminf(1.0f, pixel2.y));
    pixel2.z = fmaxf(0.0f, fminf(1.0f, pixel2.z));
    
    // Convert to YUV using Rec.709 coefficients
    float y1 = 0.2126f * pixel1.x + 0.7152f * pixel1.y + 0.0722f * pixel1.z;
    float y2 = 0.2126f * pixel2.x + 0.7152f * pixel2.y + 0.0722f * pixel2.z;
    
    float avgR = (pixel1.x + pixel2.x) * 0.5f;
    float avgG = (pixel1.y + pixel2.y) * 0.5f;
    float avgB = (pixel1.z + pixel2.z) * 0.5f;
    
    float u = -0.1146f * avgR - 0.3854f * avgG + 0.5f * avgB;
    float v = 0.5f * avgR - 0.4542f * avgG - 0.0458f * avgB;
    
    // Scale to 8-bit and pack as UYVY
    int dstIdx = y * (width / 2) + (x / 2);
    uyvyOutput[dstIdx] = make_uchar4(
        (unsigned char)((u + 0.5f) * 255.0f),  // U
        (unsigned char)(y1 * 255.0f),          // Y1
        (unsigned char)((v + 0.5f) * 255.0f),  // V
        (unsigned char)(y2 * 255.0f)           // Y2
    );
}

// CUDA kernel for RGBA to HDR P216 conversion
__global__ void rgba_to_hdr_p216_kernel(
    const float4* __restrict__ rgbaInput,
    unsigned short* __restrict__ yPlaneOutput,
    unsigned short* __restrict__ uvPlaneOutput,
    int width,
    int height,
    float scale
) {
    int x = (blockIdx.x * blockDim.x + threadIdx.x) * 2; // Process two pixels for 4:2:2
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    
    if (x >= width || y >= height) return;
    
    // Flip vertically: OpenFX uses bottom-left origin, NDI expects top-left
    int srcRow = height - 1 - y;
    
    // Read two RGBA pixels
    int srcIdx1 = srcRow * width + x;
    int srcIdx2 = srcRow * width + x + 1;
    
    float4 rgba1 = rgbaInput[srcIdx1];
    float4 rgba2 = (x + 1 < width) ? rgbaInput[srcIdx2] : rgba1;
    
    // Clamp to 0-1 range
    rgba1.x = fmaxf(0.0f, fminf(1.0f, rgba1.x));
    rgba1.y = fmaxf(0.0f, fminf(1.0f, rgba1.y));
    rgba1.z = fmaxf(0.0f, fminf(1.0f, rgba1.z));
    
    rgba2.x = fmaxf(0.0f, fminf(1.0f, rgba2.x));
    rgba2.y = fmaxf(0.0f, fminf(1.0f, rgba2.y));
    rgba2.z = fmaxf(0.0f, fminf(1.0f, rgba2.z));
    
    // Convert to YUV using Rec.2020 coefficients for HDR
    float y1 = 0.2627f * rgba1.x + 0.6780f * rgba1.y + 0.0593f * rgba1.z;
    float y2 = 0.2627f * rgba2.x + 0.6780f * rgba2.y + 0.0593f * rgba2.z;
    
    // Average chroma for 4:2:2 subsampling
    float avgR = (rgba1.x + rgba2.x) * 0.5f;
    float avgG = (rgba1.y + rgba2.y) * 0.5f;
    float avgB = (rgba1.z + rgba2.z) * 0.5f;
    
    float u = -0.1396f * avgR - 0.3604f * avgG + 0.5f * avgB;
    float v = 0.5f * avgR - 0.4598f * avgG - 0.0402f * avgB;
    
    // Convert to 16-bit limited range (ITU BT.2100)
    // Y: 16-bit limited range [4096, 60160] for 10-bit equivalent [64, 940]
    // UV: 16-bit limited range [4096, 61440] for 10-bit equivalent [64, 960]
    unsigned short y1_16 = (unsigned short)(4096 + y1 * 56064); // (60160-4096)
    unsigned short y2_16 = (unsigned short)(4096 + y2 * 56064);
    unsigned short u_16 = (unsigned short)(32768 + u * 28672); // Center + range
    unsigned short v_16 = (unsigned short)(32768 + v * 28672);
    
    // Store in P216 format (planar)
    int yIdx1 = y * width + x;
    int yIdx2 = y * width + x + 1;
    int uvIdx = (y * width + x) / 2; // 4:2:2 subsampling
    
    yPlaneOutput[yIdx1] = y1_16;
    if (x + 1 < width) {
        yPlaneOutput[yIdx2] = y2_16;
    }
    
    // Store U and V interleaved for 4:2:2
    uvPlaneOutput[uvIdx * 2] = u_16;     // U
    uvPlaneOutput[uvIdx * 2 + 1] = v_16; // V
}

bool cuda_gpu_is_available(void) {
    int deviceCount = 0;
    cudaError_t error = cudaGetDeviceCount(&deviceCount);
    
    if (error != cudaSuccess || deviceCount == 0) {
        CUDA_LOG("CUDA not available: %s", cudaGetErrorString(error));
        return false;
    }
    
    // Check if we have at least one device with compute capability 3.0+
    for (int i = 0; i < deviceCount; i++) {
        cudaDeviceProp props;
        if (cudaGetDeviceProperties(&props, i) == cudaSuccess) {
            if (props.major >= 3) {
                CUDA_LOG("Found CUDA device %d: %s (Compute %d.%d)", 
                        i, props.name, props.major, props.minor);
                return true;
            }
        }
    }
    
    CUDA_LOG("No suitable CUDA devices found (need compute capability 3.0+)");
    return false;
}

CudaGPUContextRef cuda_gpu_init(void) {
    CUDA_LOG("Initializing CUDA GPU acceleration...");
    
    if (!cuda_gpu_is_available()) {
        return nullptr;
    }
    
    CudaGPUContext* context = (CudaGPUContext*)malloc(sizeof(CudaGPUContext));
    if (!context) {
        CUDA_LOG("Failed to allocate CUDA context");
        return nullptr;
    }
    
    memset(context, 0, sizeof(CudaGPUContext));
    
    // Find the best GPU (highest compute capability)
    int deviceCount = 0;
    cudaGetDeviceCount(&deviceCount);
    
    int bestDevice = 0;
    int bestMajor = 0, bestMinor = 0;
    
    for (int i = 0; i < deviceCount; i++) {
        cudaDeviceProp props;
        if (cudaGetDeviceProperties(&props, i) == cudaSuccess) {
            if (props.major > bestMajor || 
                (props.major == bestMajor && props.minor > bestMinor)) {
                bestDevice = i;
                bestMajor = props.major;
                bestMinor = props.minor;
            }
        }
    }
    
    context->deviceId = bestDevice;
    
    // Set device
    if (cudaSetDevice(context->deviceId) != cudaSuccess) {
        CUDA_LOG("Failed to set CUDA device %d", context->deviceId);
        free(context);
        return nullptr;
    }
    
    // Get device properties
    if (cudaGetDeviceProperties(&context->deviceProps, context->deviceId) != cudaSuccess) {
        CUDA_LOG("Failed to get device properties");
        free(context);
        return nullptr;
    }
    
    strncpy_s(context->deviceName, sizeof(context->deviceName), 
              context->deviceProps.name, sizeof(context->deviceName) - 1);
    
    // Create CUDA stream for asynchronous operations
    if (cudaStreamCreate(&context->stream) != cudaSuccess) {
        CUDA_LOG("Failed to create CUDA stream");
        free(context);
        return nullptr;
    }
    
    CUDA_LOG("CUDA GPU acceleration initialized successfully");
    CUDA_LOG("Device: %s (Compute %d.%d)", 
            context->deviceName, 
            context->deviceProps.major, 
            context->deviceProps.minor);
    CUDA_LOG("Global Memory: %.1f MB", 
            context->deviceProps.totalGlobalMem / (1024.0f * 1024.0f));
    
    return context;
}

void cuda_gpu_shutdown(CudaGPUContextRef context) {
    if (!context) return;
    
    CUDA_LOG("Shutting down CUDA GPU acceleration...");
    
    // Free device memory
    if (context->d_rgbaInput) {
        CUDA_CHECK_VOID(cudaFree(context->d_rgbaInput));
    }
    if (context->d_uyvyOutput) {
        CUDA_CHECK_VOID(cudaFree(context->d_uyvyOutput));
    }
    if (context->d_hdrOutput) {
        CUDA_CHECK_VOID(cudaFree(context->d_hdrOutput));
    }
    
    // Destroy stream
    if (context->stream) {
        CUDA_CHECK_VOID(cudaStreamDestroy(context->stream));
    }
    
    // Reset device
    CUDA_CHECK_VOID(cudaDeviceReset());
    
    free(context);
}

bool cuda_gpu_convert_rgba_to_uyvy(CudaGPUContextRef context, 
                                   const float* rgbaData, 
                                   unsigned char* uyvyData,
                                   int width, 
                                   int height) {
    if (!context || !rgbaData || !uyvyData) {
        CUDA_LOG("Invalid parameters for RGBA to UYVY conversion");
        return false;
    }
    
    auto startTime = std::chrono::high_resolution_clock::now();
    
    CUDA_LOG("Starting CUDA RGBA->UYVY conversion (%dx%d)", width, height);
    
    size_t inputSize = width * height * 4 * sizeof(float);
    size_t outputSize = width * height * 2; // UYVY is 2 bytes per pixel
    
    // Allocate or reallocate device memory if needed
    if (context->allocatedInputSize < inputSize) {
        if (context->d_rgbaInput) {
            CUDA_CHECK(cudaFree(context->d_rgbaInput));
        }
        CUDA_CHECK(cudaMalloc(&context->d_rgbaInput, inputSize));
        context->allocatedInputSize = inputSize;
    }
    
    if (context->allocatedUyvySize < outputSize) {
        if (context->d_uyvyOutput) {
            CUDA_CHECK(cudaFree(context->d_uyvyOutput));
        }
        CUDA_CHECK(cudaMalloc(&context->d_uyvyOutput, outputSize));
        context->allocatedUyvySize = outputSize;
    }
    
    // Copy input data to device
    CUDA_CHECK(cudaMemcpyAsync(context->d_rgbaInput, rgbaData, inputSize, 
                              cudaMemcpyHostToDevice, context->stream));
    
    // Configure kernel launch parameters
    dim3 blockSize(16, 16);
    dim3 gridSize((width / 2 + blockSize.x - 1) / blockSize.x, 
                  (height + blockSize.y - 1) / blockSize.y);
    
    // Launch kernel
    rgba_to_uyvy_kernel<<<gridSize, blockSize, 0, context->stream>>>(
        (float4*)context->d_rgbaInput,
        (uchar4*)context->d_uyvyOutput,
        width,
        height
    );
    
    // Check for kernel launch errors
    cudaError_t kernelError = cudaGetLastError();
    if (kernelError != cudaSuccess) {
        CUDA_LOG("CUDA kernel launch failed: %s", cudaGetErrorString(kernelError));
        return false;
    }
    
    // Copy result back to host
    CUDA_CHECK(cudaMemcpyAsync(uyvyData, context->d_uyvyOutput, outputSize, 
                              cudaMemcpyDeviceToHost, context->stream));
    
    // Wait for completion
    CUDA_CHECK(cudaStreamSynchronize(context->stream));
    
    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);
    
    CUDA_LOG("🚀 CUDA RGBA->UYVY conversion completed in %lld μs (%.2f ms)", 
           duration.count(), duration.count() / 1000.0);
    
    return true;
}

bool cuda_gpu_convert_rgba_to_hdr(CudaGPUContextRef context,
                                  const float* rgbaData,
                                  unsigned short* hdrData,
                                  int width,
                                  int height,
                                  float scale) {
    if (!context || !rgbaData || !hdrData) {
        CUDA_LOG("Invalid parameters for RGBA to HDR conversion");
        return false;
    }
    
    auto startTime = std::chrono::high_resolution_clock::now();
    
    CUDA_LOG("Starting CUDA RGBA->HDR conversion (%dx%d)", width, height);
    
    size_t inputSize = width * height * 4 * sizeof(float);
    size_t outputSize = width * height * 2 * sizeof(unsigned short); // P216 format
    
    // Allocate or reallocate device memory if needed
    if (context->allocatedInputSize < inputSize) {
        if (context->d_rgbaInput) {
            CUDA_CHECK(cudaFree(context->d_rgbaInput));
        }
        CUDA_CHECK(cudaMalloc(&context->d_rgbaInput, inputSize));
        context->allocatedInputSize = inputSize;
    }
    
    if (context->allocatedHdrSize < outputSize) {
        if (context->d_hdrOutput) {
            CUDA_CHECK(cudaFree(context->d_hdrOutput));
        }
        CUDA_CHECK(cudaMalloc(&context->d_hdrOutput, outputSize));
        context->allocatedHdrSize = outputSize;
    }
    
    // Copy input data to device
    CUDA_CHECK(cudaMemcpyAsync(context->d_rgbaInput, rgbaData, inputSize, 
                              cudaMemcpyHostToDevice, context->stream));
    
    // Configure kernel launch parameters
    dim3 blockSize(16, 16);
    dim3 gridSize((width / 2 + blockSize.x - 1) / blockSize.x, 
                  (height + blockSize.y - 1) / blockSize.y);
    
    // Calculate plane pointers for P216 format
    unsigned short* yPlane = context->d_hdrOutput;
    unsigned short* uvPlane = context->d_hdrOutput + (width * height);
    
    // Launch kernel
    rgba_to_hdr_p216_kernel<<<gridSize, blockSize, 0, context->stream>>>(
        (float4*)context->d_rgbaInput,
        yPlane,
        uvPlane,
        width,
        height,
        scale
    );
    
    // Check for kernel launch errors
    cudaError_t kernelError = cudaGetLastError();
    if (kernelError != cudaSuccess) {
        CUDA_LOG("CUDA HDR kernel launch failed: %s", cudaGetErrorString(kernelError));
        return false;
    }
    
    // Copy result back to host
    CUDA_CHECK(cudaMemcpyAsync(hdrData, context->d_hdrOutput, outputSize, 
                              cudaMemcpyDeviceToHost, context->stream));
    
    // Wait for completion
    CUDA_CHECK(cudaStreamSynchronize(context->stream));
    
    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);
    
    CUDA_LOG("🚀 CUDA RGBA->HDR conversion completed in %lld μs (%.2f ms)", 
           duration.count(), duration.count() / 1000.0);
    
    return true;
}

const char* cuda_gpu_get_device_name(CudaGPUContextRef context) {
    if (!context) return "Unknown";
    return context->deviceName;
}

bool cuda_gpu_get_memory_info(CudaGPUContextRef context, size_t* free_mem, size_t* total_mem) {
    if (!context || !free_mem || !total_mem) return false;
    
    if (cudaSetDevice(context->deviceId) != cudaSuccess) {
        return false;
    }
    
    return cudaMemGetInfo(free_mem, total_mem) == cudaSuccess;
}

#endif // Windows 