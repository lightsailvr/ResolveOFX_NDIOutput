#ifdef __APPLE__

#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <Foundation/Foundation.h>
#import <os/log.h>
#include "MetalGPUAcceleration.h"
#include <stdio.h>
#include <chrono>

#define METAL_LOG(fmt, ...) os_log(OS_LOG_DEFAULT, "NDI Plugin Metal: " fmt, ##__VA_ARGS__)

// Metal GPU Context structure
struct MetalGPUContext {
    id<MTLDevice> device;
    id<MTLCommandQueue> commandQueue;
    id<MTLComputePipelineState> rgbaToUyvyPipeline;
    id<MTLComputePipelineState> rgbaToHdrPipeline;
    id<MTLLibrary> library;
};

// Metal shader source code
static const char* metalShaderSource = R"(
#include <metal_stdlib>
using namespace metal;

// RGBA to UYVY conversion kernel
kernel void rgba_to_uyvy_kernel(device const float4* rgbaInput [[buffer(0)]],
                                device uchar4* uyvyOutput [[buffer(1)]],
                                constant uint2& dimensions [[buffer(2)]],
                                uint2 gid [[thread_position_in_grid]]) {
    
    uint width = dimensions.x;
    uint height = dimensions.y;
    
    // Process two pixels at a time for UYVY (4:2:2 format)
    if (gid.x * 2 >= width || gid.y >= height) return;
    
    uint x = gid.x * 2;
    uint y = gid.y;
    
    // Flip vertically: OpenFX uses bottom-left origin, NDI expects top-left
    uint srcRow = height - 1 - y;
    
    // Get two adjacent pixels
    uint srcIdx1 = srcRow * width + x;
    uint srcIdx2 = srcRow * width + x + 1;
    
    float4 pixel1 = rgbaInput[srcIdx1];
    float4 pixel2 = (x + 1 < width) ? rgbaInput[srcIdx2] : pixel1;
    
    // Clamp values to [0, 1]
    pixel1 = clamp(pixel1, 0.0f, 1.0f);
    pixel2 = clamp(pixel2, 0.0f, 1.0f);
    
    // Convert to YUV using Rec.709 coefficients
    float y1 = 0.2126f * pixel1.r + 0.7152f * pixel1.g + 0.0722f * pixel1.b;
    float y2 = 0.2126f * pixel2.r + 0.7152f * pixel2.g + 0.0722f * pixel2.b;
    
    float avgR = (pixel1.r + pixel2.r) * 0.5f;
    float avgG = (pixel1.g + pixel2.g) * 0.5f;
    float avgB = (pixel1.b + pixel2.b) * 0.5f;
    
    float u = -0.1146f * avgR - 0.3854f * avgG + 0.5f * avgB;
    float v = 0.5f * avgR - 0.4542f * avgG - 0.0458f * avgB;
    
    // Scale to 8-bit and pack as UYVY
    uint dstIdx = y * (width / 2) + gid.x;
    uyvyOutput[dstIdx] = uchar4(
        uchar((u + 0.5f) * 255.0f),  // U
        uchar(y1 * 255.0f),          // Y1
        uchar((v + 0.5f) * 255.0f),  // V
        uchar(y2 * 255.0f)           // Y2
    );
}

// RGBA to HDR P216 conversion kernel
kernel void rgba_to_hdr_p216(
    const device float4* rgbaInput [[buffer(0)]],
    device uint16_t* yPlaneOutput [[buffer(1)]],
    device uint16_t* uvPlaneOutput [[buffer(2)]],
    constant uint2& dimensions [[buffer(3)]],
    constant float& scale [[buffer(4)]],
    uint2 gid [[thread_position_in_grid]]
) {
    if (gid.x >= dimensions.x || gid.y >= dimensions.y) {
        return;
    }
    
    // Process two pixels for 4:2:2 subsampling
    uint x = gid.x * 2;
    uint y = gid.y;
    
    if (x >= dimensions.x) {
        return;
    }
    
    // Flip vertically: OpenFX uses bottom-left origin, NDI expects top-left
    uint srcRow = dimensions.y - 1 - y;
    
    // Read two RGBA pixels
    uint srcIdx1 = srcRow * dimensions.x + x;
    uint srcIdx2 = srcRow * dimensions.x + x + 1;
    
    float4 rgba1 = rgbaInput[srcIdx1];
    float4 rgba2 = (x + 1 < dimensions.x) ? rgbaInput[srcIdx2] : rgba1;
    
    // Clamp to 0-1 range
    rgba1 = clamp(rgba1, 0.0f, 1.0f);
    rgba2 = clamp(rgba2, 0.0f, 1.0f);
    
    // Convert to YUV using Rec.2020 coefficients for HDR
    float y1 = 0.2627f * rgba1.r + 0.6780f * rgba1.g + 0.0593f * rgba1.b;
    float y2 = 0.2627f * rgba2.r + 0.6780f * rgba2.g + 0.0593f * rgba2.b;
    
    // Average chroma for 4:2:2 subsampling
    float3 avgRGB = (rgba1.rgb + rgba2.rgb) * 0.5f;
    float u = -0.1396f * avgRGB.r - 0.3604f * avgRGB.g + 0.5f * avgRGB.b;
    float v = 0.5f * avgRGB.r - 0.4598f * avgRGB.g - 0.0402f * avgRGB.b;
    
    // Convert to 16-bit limited range (ITU BT.2100)
    // Y: 16-bit limited range [4096, 60160] for 10-bit equivalent [64, 940]
    // UV: 16-bit limited range [4096, 61440] for 10-bit equivalent [64, 960]
    uint16_t y1_16 = uint16_t(4096 + y1 * 56064); // (60160-4096)
    uint16_t y2_16 = uint16_t(4096 + y2 * 56064);
    uint16_t u_16 = uint16_t(32768 + u * 28672); // Center + range
    uint16_t v_16 = uint16_t(32768 + v * 28672);
    
    // Store in P216 format (planar)
    uint yIdx1 = y * dimensions.x + x;
    uint yIdx2 = y * dimensions.x + x + 1;
    uint uvIdx = (y * dimensions.x + x) / 2; // 4:2:2 subsampling
    
    yPlaneOutput[yIdx1] = y1_16;
    if (x + 1 < dimensions.x) {
        yPlaneOutput[yIdx2] = y2_16;
    }
    
    // Store U and V interleaved for 4:2:2
    uvPlaneOutput[uvIdx * 2] = u_16;     // U
    uvPlaneOutput[uvIdx * 2 + 1] = v_16; // V
}
)";

bool metal_gpu_is_available(void) {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        return device != nil;
    }
}

MetalGPUContextRef metal_gpu_init(void) {
    @autoreleasepool {
        METAL_LOG("Initializing Metal GPU acceleration...\n");
        
        // Create Metal device
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            METAL_LOG("Failed to create Metal device\n");
            return nullptr;
        }
        
        METAL_LOG("Metal device: %s\n", [[device name] UTF8String]);
        
        // Create command queue
        id<MTLCommandQueue> commandQueue = [device newCommandQueue];
        if (!commandQueue) {
            METAL_LOG("Failed to create Metal command queue\n");
            return nullptr;
        }
        
        // Create library from source
        NSString* shaderSource = [NSString stringWithUTF8String:metalShaderSource];
        NSError* error = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:shaderSource options:nil error:&error];
        if (!library) {
            METAL_LOG("Failed to create Metal library: %s\n", [[error localizedDescription] UTF8String]);
            return nullptr;
        }
        
        // Create compute pipeline states
        id<MTLFunction> rgbaToUyvyFunction = [library newFunctionWithName:@"rgba_to_uyvy_kernel"];
        id<MTLFunction> rgbaToHdrFunction = [library newFunctionWithName:@"rgba_to_hdr_p216"];
        
        if (!rgbaToUyvyFunction || !rgbaToHdrFunction) {
            METAL_LOG("Failed to find Metal kernel functions\n");
            return nullptr;
        }
        
        id<MTLComputePipelineState> rgbaToUyvyPipeline = [device newComputePipelineStateWithFunction:rgbaToUyvyFunction error:&error];
        if (!rgbaToUyvyPipeline) {
            METAL_LOG("Failed to create RGBA to UYVY pipeline: %s\n", [[error localizedDescription] UTF8String]);
            return nullptr;
        }
        
        id<MTLComputePipelineState> rgbaToHdrPipeline = [device newComputePipelineStateWithFunction:rgbaToHdrFunction error:&error];
        if (!rgbaToHdrPipeline) {
            METAL_LOG("Failed to create RGBA to HDR pipeline: %s\n", [[error localizedDescription] UTF8String]);
            return nullptr;
        }
        
        // Create context
        MetalGPUContext* context = new MetalGPUContext;
        context->device = device;
        context->commandQueue = commandQueue;
        context->rgbaToUyvyPipeline = rgbaToUyvyPipeline;
        context->rgbaToHdrPipeline = rgbaToHdrPipeline;
        context->library = library;
        
        // Retain objects
        [device retain];
        [commandQueue retain];
        [rgbaToUyvyPipeline retain];
        [rgbaToHdrPipeline retain];
        [library retain];
        
        METAL_LOG("Metal GPU acceleration initialized successfully\n");
        return context;
    }
}

void metal_gpu_shutdown(MetalGPUContextRef context) {
    if (!context) return;
    
    @autoreleasepool {
        METAL_LOG("Shutting down Metal GPU acceleration...\n");
        
        [context->device release];
        [context->commandQueue release];
        [context->rgbaToUyvyPipeline release];
        [context->rgbaToHdrPipeline release];
        [context->library release];
        
        delete context;
    }
}

bool metal_gpu_convert_rgba_to_uyvy(MetalGPUContextRef context, 
                                   const float* rgbaData, 
                                   unsigned char* uyvyData,
                                   int width, 
                                   int height) {
    if (!context || !rgbaData || !uyvyData) return false;
    
    @autoreleasepool {
        // Start timing
        auto startTime = std::chrono::high_resolution_clock::now();
        
        METAL_LOG("Starting Metal GPU RGBA->UYVY conversion (%dx%d)\n", width, height);
        
        // Create buffers
        size_t rgbaSize = width * height * 4 * sizeof(float);
        size_t uyvySize = width * height * 2; // UYVY is 2 bytes per pixel
        
        id<MTLBuffer> rgbaBuffer = [context->device newBufferWithBytes:rgbaData 
                                                               length:rgbaSize 
                                                              options:MTLResourceStorageModeShared];
        
        id<MTLBuffer> uyvyBuffer = [context->device newBufferWithLength:uyvySize 
                                                                options:MTLResourceStorageModeShared];
        
        uint32_t dimensions[2] = {(uint32_t)width, (uint32_t)height};
        id<MTLBuffer> dimensionsBuffer = [context->device newBufferWithBytes:dimensions 
                                                                      length:sizeof(dimensions) 
                                                                     options:MTLResourceStorageModeShared];
        
        if (!rgbaBuffer || !uyvyBuffer || !dimensionsBuffer) {
            METAL_LOG("Failed to create Metal buffers\n");
            return false;
        }
        
        // Create command buffer and encoder
        id<MTLCommandBuffer> commandBuffer = [context->commandQueue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
        
        [encoder setComputePipelineState:context->rgbaToUyvyPipeline];
        [encoder setBuffer:rgbaBuffer offset:0 atIndex:0];
        [encoder setBuffer:uyvyBuffer offset:0 atIndex:1];
        [encoder setBuffer:dimensionsBuffer offset:0 atIndex:2];
        
        // Calculate thread groups
        MTLSize threadsPerThreadgroup = MTLSizeMake(16, 16, 1);
        MTLSize threadgroupsPerGrid = MTLSizeMake(
            (width / 2 + threadsPerThreadgroup.width - 1) / threadsPerThreadgroup.width,
            (height + threadsPerThreadgroup.height - 1) / threadsPerThreadgroup.height,
            1
        );
        
        METAL_LOG("Dispatching %zu x %zu threadgroups with %zu x %zu threads each\n", 
               threadgroupsPerGrid.width, threadgroupsPerGrid.height,
               threadsPerThreadgroup.width, threadsPerThreadgroup.height);
        
        [encoder dispatchThreadgroups:threadgroupsPerGrid threadsPerThreadgroup:threadsPerThreadgroup];
        [encoder endEncoding];
        
        // Execute and wait
        [commandBuffer commit];
        [commandBuffer waitUntilCompleted];
        
        // Check for errors
        if (commandBuffer.error) {
            METAL_LOG("Metal command buffer error: %s\n", 
                   [[commandBuffer.error localizedDescription] UTF8String]);
            [rgbaBuffer release];
            [uyvyBuffer release];
            [dimensionsBuffer release];
            return false;
        }
        
        // Copy result back
        memcpy(uyvyData, [uyvyBuffer contents], uyvySize);
        
        // Calculate timing
        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);
        
        METAL_LOG("✅ Metal GPU RGBA->UYVY conversion completed in %lld μs (%.2f ms)\n", 
               duration.count(), duration.count() / 1000.0);
        
        [rgbaBuffer release];
        [uyvyBuffer release];
        [dimensionsBuffer release];
        
        return true;
    }
}

bool metal_gpu_convert_rgba_to_hdr(MetalGPUContextRef context, const float* rgbaInput, uint16_t* hdrOutput, int width, int height, float scale) {
    auto startTime = std::chrono::high_resolution_clock::now();
    
    METAL_LOG("🚀 Starting Metal HDR P216 conversion (%dx%d)", width, height);
    
    MetalGPUContext* ctx = static_cast<MetalGPUContext*>(context);
    if (!ctx || !ctx->device || !ctx->rgbaToHdrPipeline) {
        METAL_LOG("❌ Invalid Metal context for HDR conversion");
        return false;
    }
    
    @autoreleasepool {
        // Calculate buffer sizes for P216 format
        size_t inputSize = width * height * 4 * sizeof(float);
        size_t yPlaneSize = width * height * sizeof(uint16_t);
        size_t uvPlaneSize = width * height * sizeof(uint16_t); // U and V interleaved, 4:2:2 subsampling
        
        // Create input buffer
        id<MTLBuffer> inputBuffer = [ctx->device newBufferWithBytes:rgbaInput 
                                                             length:inputSize 
                                                            options:MTLResourceStorageModeShared];
        if (!inputBuffer) {
            METAL_LOG("❌ Failed to create input buffer");
            return false;
        }
        
        // Create output buffers for Y and UV planes
        id<MTLBuffer> yPlaneBuffer = [ctx->device newBufferWithLength:yPlaneSize 
                                                              options:MTLResourceStorageModeShared];
        id<MTLBuffer> uvPlaneBuffer = [ctx->device newBufferWithLength:uvPlaneSize 
                                                               options:MTLResourceStorageModeShared];
        
        if (!yPlaneBuffer || !uvPlaneBuffer) {
            METAL_LOG("❌ Failed to create output buffers");
            return false;
        }
        
        // Create parameter buffers
        simd_uint2 dimensions = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
        id<MTLBuffer> dimensionsBuffer = [ctx->device newBufferWithBytes:&dimensions 
                                                                  length:sizeof(dimensions) 
                                                                 options:MTLResourceStorageModeShared];
        id<MTLBuffer> scaleBuffer = [ctx->device newBufferWithBytes:&scale 
                                                             length:sizeof(scale) 
                                                            options:MTLResourceStorageModeShared];
        
        // Create command buffer and encoder
        id<MTLCommandBuffer> commandBuffer = [ctx->commandQueue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
        
        // Set pipeline state and buffers
        [encoder setComputePipelineState:ctx->rgbaToHdrPipeline];
        [encoder setBuffer:inputBuffer offset:0 atIndex:0];
        [encoder setBuffer:yPlaneBuffer offset:0 atIndex:1];
        [encoder setBuffer:uvPlaneBuffer offset:0 atIndex:2];
        [encoder setBuffer:dimensionsBuffer offset:0 atIndex:3];
        [encoder setBuffer:scaleBuffer offset:0 atIndex:4];
        
        // Calculate thread groups for 4:2:2 processing (process width/2 pixels horizontally)
        MTLSize threadsPerThreadgroup = MTLSizeMake(16, 16, 1);
        MTLSize threadgroupsPerGrid = MTLSizeMake(
            (width / 2 + threadsPerThreadgroup.width - 1) / threadsPerThreadgroup.width,
            (height + threadsPerThreadgroup.height - 1) / threadsPerThreadgroup.height,
            1
        );
        
        METAL_LOG("Dispatching thread groups: %lux%lu, threads per group: %lux%lu", 
                 threadgroupsPerGrid.width, threadgroupsPerGrid.height,
                 threadsPerThreadgroup.width, threadsPerThreadgroup.height);
        
        [encoder dispatchThreadgroups:threadgroupsPerGrid threadsPerThreadgroup:threadsPerThreadgroup];
        [encoder endEncoding];
        
        // Commit and wait
        [commandBuffer commit];
        [commandBuffer waitUntilCompleted];
        
        if (commandBuffer.error) {
            METAL_LOG("❌ Metal command execution failed: %s", commandBuffer.error.localizedDescription.UTF8String);
            return false;
        }
        
        // Copy results back to output buffer
        // P216 format: Y plane followed by interleaved UV plane
        uint16_t* yPlaneData = static_cast<uint16_t*>([yPlaneBuffer contents]);
        uint16_t* uvPlaneData = static_cast<uint16_t*>([uvPlaneBuffer contents]);
        
        // Copy Y plane
        memcpy(hdrOutput, yPlaneData, yPlaneSize);
        
        // Copy UV plane (starts after Y plane)
        memcpy(hdrOutput + (width * height), uvPlaneData, uvPlaneSize);
        
        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);
        
        METAL_LOG("✅ Metal HDR P216 conversion completed in %lld μs (%.2f ms)", 
                 duration.count(), duration.count() / 1000.0);
        
        return true;
    }
}

#endif // __APPLE__ 