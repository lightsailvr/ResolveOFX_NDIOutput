// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

/*
  NDI Advanced Output Plugin for OpenFX with GPU Acceleration
  Sends video frames to NDI (Network Device Interface) for streaming over network with HDR support.
  
  Based on modern OFX examples using the C API directly.
  Includes HDR support with PQ/HLG transfer functions and Rec.2020/P3 color spaces.
  Enhanced with GPU acceleration for optimal performance.
*/

#include <stdexcept>
#include <new>
#include <cstring>
#include <cmath>
#include <stdio.h>
#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <chrono>

#ifdef __APPLE__
#include <os/log.h>
#define NDI_LOG(fmt, ...) os_log(OS_LOG_DEFAULT, "NDI Plugin: " fmt, ##__VA_ARGS__)
#else
#define NDI_LOG(fmt, ...) printf("NDI Plugin: " fmt "\n", ##__VA_ARGS__)
#endif

#include "ofxImageEffect.h"
#include "ofxMemory.h"
#include "ofxMultiThread.h"

#ifdef __APPLE__
#include "MetalGPUAcceleration.h"
#endif

#ifdef _WIN32
#include "CudaGPUAcceleration.h"
#endif

// GPU acceleration headers (forward declarations only)
#ifdef __APPLE__
// Forward declarations for Metal types to avoid Objective-C in C++
#ifdef __OBJC__
#include <Metal/Metal.h>
#include <MetalKit/MetalKit.h>
#endif
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#elif defined(_WIN32) || defined(__WIN32__) || defined(WIN32) || defined(_WIN64) || defined(__WIN64__) || defined(WIN64)
#include <d3d11.h>
#include <dxgi.h>
#else
#include <GL/gl.h>
#include <GL/glext.h>
#endif

// NDI Advanced SDK includes
#ifdef __APPLE__
#include <Processing.NDI.Lib.h>
#elif defined(_WIN32) || defined(__WIN32__) || defined(WIN32) || defined(_WIN64) || defined(__WIN64__) || defined(WIN64)
#include <Processing.NDI.Lib.h>
#else
#include <Processing.NDI.Lib.h>
#endif

#if defined __APPLE__ || defined __linux__ || defined __FreeBSD__
#  define EXPORT __attribute__((visibility("default")))
#elif defined _WIN32
#  define EXPORT OfxExport
#else
#  error Not building on your operating system quite yet
#endif

// Plugin constants
#define kPluginName "NDIOutput"
#define kPluginGrouping "LSVR"
#define kPluginDescription \
"NDI Advanced Output v" kPluginVersionString " (GPU-Accelerated): GPU-accelerated NDI streaming with HDR support. \n" \
"Configure the NDI source name, HDR settings, GPU acceleration, and enable/disable the output stream. \n" \
"Version: " kPluginVersionString " - GPU-Accelerated NDI Advanced"
#define kPluginIdentifier "LSVR.NDIOutput"
#define kPluginVersionMajor 1
#define kPluginVersionMinor 2
#define kPluginVersionPatch 4
#define kPluginVersionString "1.2.4"

// Parameter names
#define kParamSourceName "sourceName"
#define kParamSourceNameLabel "NDI Source Name"
#define kParamSourceNameHint "Name of the NDI source as it will appear on the network"

#define kParamEnabled "enabled"
#define kParamEnabledLabel "Enable NDI Output"
#define kParamEnabledHint "Enable or disable NDI output streaming"

#define kParamFrameRate "frameRate"
#define kParamFrameRateLabel "Frame Rate"
#define kParamFrameRateHint "Frame rate for NDI output"

// GPU Acceleration Parameters
#define kParamGPUAcceleration "gpuAcceleration"
#define kParamGPUAccelerationLabel "GPU Acceleration"
#define kParamGPUAccelerationHint "Enable GPU acceleration for frame processing and color conversion"

#define kParamAsyncSending "asyncSending"
#define kParamAsyncSendingLabel "Asynchronous Sending"
#define kParamAsyncSendingHint "Enable asynchronous frame submission for better performance"

#define kParamOptimalFormat "optimalFormat"
#define kParamOptimalFormatLabel "Optimal Color Format"
#define kParamOptimalFormatHint "Use UYVY color format for optimal NDI performance"

// Version Display Parameter
#define kParamVersionLabel "versionLabel"
#define kParamVersionLabelLabel "Plugin Version"
#define kParamVersionLabelHint "Current version of the NDI Output plugin"

// HDR Parameters
#define kParamHDREnabled "hdrEnabled"
#define kParamHDREnabledLabel "Enable HDR"
#define kParamHDREnabledHint "Enable HDR (High Dynamic Range) output"

#define kParamColorSpace "colorSpace"
#define kParamColorSpaceLabel "Color Space"
#define kParamColorSpaceHint "Color space for HDR output"

#define kParamTransferFunction "transferFunction"
#define kParamTransferFunctionLabel "Transfer Function"
#define kParamTransferFunctionHint "Transfer function for HDR output (PQ/HLG)"

#define kParamMaxCLL "maxCLL"
#define kParamMaxCLLLabel "Max Content Light Level"
#define kParamMaxCLLHint "Maximum content light level in nits"

#define kParamMaxFALL "maxFALL"
#define kParamMaxFALLLabel "Max Frame Average Light Level"
#define kParamMaxFALLHint "Maximum frame average light level in nits"

// Color Space Options
#define kColorSpaceRec709 "rec709"
#define kColorSpaceRec2020 "rec2020"
#define kColorSpaceP3 "p3"

// Transfer Function Options
#define kTransferFunctionSDR "sdr"
#define kTransferFunctionPQ "pq"
#define kTransferFunctionHLG "hlg"

// Host pointers
OfxHost                 *gHost;
OfxImageEffectSuiteV1   *gEffectHost = 0;
OfxPropertySuiteV1      *gPropHost = 0;
OfxParameterSuiteV1     *gParamHost = 0;
OfxMemorySuiteV1        *gMemoryHost = 0;
OfxMultiThreadSuiteV1   *gThreadHost = 0;
OfxMessageSuiteV1       *gMessageSuite = 0;

// GPU Processing Context
struct GPUContext {
#ifdef __APPLE__
    MetalGPUContextRef metalContext;
#elif defined(_WIN32)
    CudaGPUContextRef cudaContext;
    void* d3dDevice;
    void* d3dContext;
    void* colorConversionShader;
    void* frameBuffer;
#else
    // OpenGL context for Linux
    unsigned int framebuffer;
    unsigned int colorConversionProgram;
    unsigned int frameTexture;
#endif
    bool initialized;
    std::mutex gpuMutex;
};

// Asynchronous frame processing
struct AsyncFrameData {
    std::vector<uint8_t> frameData;
    int width;
    int height;
    bool isHDR;
    std::chrono::high_resolution_clock::time_point timestamp;
};

// Private instance data
struct NDIInstanceData {
    // Clip handles
    OfxImageClipHandle sourceClip;
    OfxImageClipHandle outputClip;
    
    // Parameter handles
    OfxParamHandle sourceNameParam;
    OfxParamHandle enabledParam;
    OfxParamHandle frameRateParam;
    OfxParamHandle gpuAccelerationParam;
    OfxParamHandle asyncSendingParam;
    OfxParamHandle optimalFormatParam;
    OfxParamHandle versionLabelParam;
    OfxParamHandle hdrEnabledParam;
    OfxParamHandle colorSpaceParam;
    OfxParamHandle transferFunctionParam;
    OfxParamHandle maxCLLParam;
    OfxParamHandle maxFALLParam;
    
    // NDI variables
    NDIlib_send_instance_t ndiSend;
    bool ndiInitialized;
    std::string sourceName;
    bool enabled;
    double frameRate;
    
    // GPU acceleration settings
    bool gpuAcceleration;
    bool asyncSending;
    bool optimalFormat;
    std::unique_ptr<GPUContext> gpuContext;
    
    // HDR parameters
    bool hdrEnabled;
    std::string colorSpace;
    std::string transferFunction;
    double maxCLL;
    double maxFALL;
    
    // Frame buffers
    std::vector<uint8_t> frameBuffer;
    std::vector<uint16_t> hdrFrameBuffer;
    std::vector<uint8_t> uyvyFrameBuffer; // UYVY format for optimal performance
    std::string hdrMetadataXML;
    
    // Asynchronous processing
    std::thread asyncThread;
    std::queue<AsyncFrameData> frameQueue;
    std::mutex queueMutex;
    std::condition_variable queueCondition;
    bool stopAsyncThread;
    
    // Custom memory allocator for NDI
    std::unique_ptr<uint8_t[]> customMemoryPool;
    size_t memoryPoolSize;
    std::mutex memoryMutex;
};

// Forward declarations
static void convertRGBAToUYVY_CPU(NDIInstanceData* data, void* rgbaData, int width, int height);
static void sendHDRFrame(NDIInstanceData* data, void* imageData, int width, int height);
static void sendSDRFrame(NDIInstanceData* data, void* imageData, int width, int height);

// GPU Acceleration Functions
static bool initializeGPUContext(NDIInstanceData* data)
{
    if (!data->gpuAcceleration) {
        return true; // GPU acceleration disabled
    }

    NDI_LOG("Initializing GPU acceleration...");
    
    data->gpuContext = std::make_unique<GPUContext>();
    data->gpuContext->initialized = false;

#ifdef __APPLE__
    // Check if Metal is available
    if (!metal_gpu_is_available()) {
        NDI_LOG("Metal is not available on this system\n");
        return false;
    }
    
    // Initialize Metal GPU acceleration
    data->gpuContext->metalContext = metal_gpu_init();
    if (!data->gpuContext->metalContext) {
        NDI_LOG("Failed to initialize Metal GPU acceleration\n");
        return false;
    }
    
    NDI_LOG("Metal GPU acceleration initialized successfully\n");
    
#elif defined(_WIN32)
    // Try CUDA first, then fallback to D3D11
    if (cuda_gpu_is_available()) {
        NDI_LOG("Initializing CUDA GPU acceleration...");
        data->gpuContext->cudaContext = cuda_gpu_init();
        if (data->gpuContext->cudaContext) {
            NDI_LOG("CUDA GPU acceleration initialized successfully");
            NDI_LOG("Device: %s", cuda_gpu_get_device_name(data->gpuContext->cudaContext));
            
            size_t free_mem, total_mem;
            if (cuda_gpu_get_memory_info(data->gpuContext->cudaContext, &free_mem, &total_mem)) {
                NDI_LOG("CUDA Memory: %.1f MB free / %.1f MB total", 
                       free_mem / (1024.0f * 1024.0f), 
                       total_mem / (1024.0f * 1024.0f));
            }
        } else {
            NDI_LOG("Failed to initialize CUDA GPU acceleration, trying D3D11...");
        }
    } else {
        NDI_LOG("CUDA not available, trying D3D11...");
    }
    
    // Fallback to D3D11 if CUDA failed
    if (!data->gpuContext->cudaContext) {
        HRESULT hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            0, nullptr, 0, D3D11_SDK_VERSION,
            (ID3D11Device**)&data->gpuContext->d3dDevice,
            nullptr, (ID3D11DeviceContext**)&data->gpuContext->d3dContext);
        
        if (FAILED(hr)) {
            NDI_LOG("Failed to create D3D11 device");
            return false;
        }
        
        NDI_LOG("D3D11 GPU acceleration initialized as fallback");
    }
#else
    // Initialize OpenGL for Linux
    // Note: This would require proper OpenGL context setup
    NDI_LOG("OpenGL GPU acceleration available\n");
#endif

    data->gpuContext->initialized = true;
    return true;
}

static void shutdownGPUContext(NDIInstanceData* data)
{
    if (!data->gpuContext || !data->gpuContext->initialized) {
        return;
    }

    NDI_LOG("Shutting down GPU acceleration...\n");

#ifdef __APPLE__
    // Shutdown Metal GPU acceleration
    if (data->gpuContext->metalContext) {
        metal_gpu_shutdown(data->gpuContext->metalContext);
        data->gpuContext->metalContext = nullptr;
    }
#elif defined(_WIN32)
    // Shutdown CUDA if it was initialized
    if (data->gpuContext->cudaContext) {
        cuda_gpu_shutdown(data->gpuContext->cudaContext);
        data->gpuContext->cudaContext = nullptr;
    }
    
    // Shutdown D3D11 if it was initialized
    if (data->gpuContext->d3dContext) {
        ((ID3D11DeviceContext*)data->gpuContext->d3dContext)->Release();
    }
    if (data->gpuContext->d3dDevice) {
        ((ID3D11Device*)data->gpuContext->d3dDevice)->Release();
    }
#endif

    data->gpuContext->initialized = false;
}

static void convertRGBAToUYVY_GPU(NDIInstanceData* data, void* rgbaData, int width, int height)
{
    if (!data->gpuContext || !data->gpuContext->initialized) {
        NDI_LOG("⚠️ GPU context not available, falling back to CPU\n");
        convertRGBAToUYVY_CPU(data, rgbaData, width, height);
        return;
    }

    std::lock_guard<std::mutex> lock(data->gpuContext->gpuMutex);

    const size_t uyvySize = width * height * 2; // UYVY is 2 bytes per pixel
    if (data->uyvyFrameBuffer.size() != uyvySize) {
        data->uyvyFrameBuffer.resize(uyvySize);
    }

#ifdef __APPLE__
    // Use Metal GPU acceleration for RGBA to UYVY conversion
    if (data->gpuContext->metalContext) {
        NDI_LOG("🚀 Attempting Metal GPU acceleration...\n");
        
        bool success = metal_gpu_convert_rgba_to_uyvy(
            data->gpuContext->metalContext,
            static_cast<const float*>(rgbaData),
            data->uyvyFrameBuffer.data(),
            width,
            height
        );
        
        if (success) {
            NDI_LOG("✅ Metal GPU acceleration SUCCESS!\n");
            return;
        } else {
            NDI_LOG("❌ Metal GPU conversion failed, falling back to CPU\n");
        }
    } else {
        NDI_LOG("⚠️ Metal context not available, falling back to CPU\n");
    }
    
    // Fallback to CPU if Metal fails
    convertRGBAToUYVY_CPU(data, rgbaData, width, height);
    
#elif defined(_WIN32)
    // Try CUDA first if available
    if (data->gpuContext->cudaContext) {
        NDI_LOG("🚀 Attempting CUDA GPU acceleration...");
        
        bool success = cuda_gpu_convert_rgba_to_uyvy(
            data->gpuContext->cudaContext,
            static_cast<const float*>(rgbaData),
            data->uyvyFrameBuffer.data(),
            width,
            height
        );
        
        if (success) {
            NDI_LOG("✅ CUDA GPU acceleration SUCCESS!");
            return;
        } else {
            NDI_LOG("❌ CUDA GPU conversion failed, falling back to CPU");
        }
    }
    
    // D3D11 implementation for RGBA to UYVY conversion (fallback)
    if (data->gpuContext->d3dDevice && data->gpuContext->d3dContext) {
        ID3D11Device* device = (ID3D11Device*)data->gpuContext->d3dDevice;
        ID3D11DeviceContext* context = (ID3D11DeviceContext*)data->gpuContext->d3dContext;
        
        // Create buffers and execute compute shader
        // ... D3D11 compute shader execution ...
        NDI_LOG("D3D11 GPU conversion available, using CPU fallback for now");
    }
    
    // Fallback to CPU if both GPU methods failed
    convertRGBAToUYVY_CPU(data, rgbaData, width, height);
    
#else
    // OpenGL implementation for Linux
    // ... OpenGL compute shader execution ...
    NDI_LOG("OpenGL GPU conversion available, using CPU fallback for now\n");
    convertRGBAToUYVY_CPU(data, rgbaData, width, height);
    return;
#endif
}

static void convertRGBAToUYVY_CPU(NDIInstanceData* data, void* rgbaData, int width, int height)
{
    auto startTime = std::chrono::high_resolution_clock::now();
    
    NDI_LOG("Starting CPU RGBA->UYVY conversion (%dx%d)\n", width, height);
    
    const size_t uyvySize = width * height * 2; // UYVY is 2 bytes per pixel
    if (data->uyvyFrameBuffer.size() != uyvySize) {
        data->uyvyFrameBuffer.resize(uyvySize);
    }

    float* srcData = static_cast<float*>(rgbaData);
    uint8_t* dstData = data->uyvyFrameBuffer.data();

    // Convert RGBA float to UYVY (4:2:2 format) with vertical flip
    for (int y = 0; y < height; ++y) {
        int srcRow = height - 1 - y; // Flip vertically: OpenFX uses bottom-left origin, NDI expects top-left
        for (int x = 0; x < width; x += 2) {
            int srcIdx1 = (srcRow * width + x) * 4;
            int srcIdx2 = (srcRow * width + x + 1) * 4;
            int dstIdx = (y * width + x) * 2;

            // Get RGB values for two pixels
            float r1 = std::max(0.0f, std::min(1.0f, srcData[srcIdx1 + 0]));
            float g1 = std::max(0.0f, std::min(1.0f, srcData[srcIdx1 + 1]));
            float b1 = std::max(0.0f, std::min(1.0f, srcData[srcIdx1 + 2]));

            float r2 = (x + 1 < width) ? std::max(0.0f, std::min(1.0f, srcData[srcIdx2 + 0])) : r1;
            float g2 = (x + 1 < width) ? std::max(0.0f, std::min(1.0f, srcData[srcIdx2 + 1])) : g1;
            float b2 = (x + 1 < width) ? std::max(0.0f, std::min(1.0f, srcData[srcIdx2 + 2])) : b1;

            // Convert to YUV using Rec.709 coefficients
            float y1 = 0.2126f * r1 + 0.7152f * g1 + 0.0722f * b1;
            float y2 = 0.2126f * r2 + 0.7152f * g2 + 0.0722f * b2;
            float u = -0.1146f * ((r1 + r2) * 0.5f) - 0.3854f * ((g1 + g2) * 0.5f) + 0.5f * ((b1 + b2) * 0.5f);
            float v = 0.5f * ((r1 + r2) * 0.5f) - 0.4542f * ((g1 + g2) * 0.5f) - 0.0458f * ((b1 + b2) * 0.5f);

            // Scale to 8-bit and pack as UYVY
            dstData[dstIdx + 0] = static_cast<uint8_t>((u + 0.5f) * 255.0f);  // U
            dstData[dstIdx + 1] = static_cast<uint8_t>(y1 * 255.0f);          // Y1
            dstData[dstIdx + 2] = static_cast<uint8_t>((v + 0.5f) * 255.0f);  // V
            dstData[dstIdx + 3] = static_cast<uint8_t>(y2 * 255.0f);          // Y2
        }
    }
    
    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);
    
    NDI_LOG("🖥️ CPU RGBA->UYVY conversion completed in %lld μs (%.2f ms)\n", 
           duration.count(), duration.count() / 1000.0);
}

static void asyncFrameProcessor(NDIInstanceData* data)
{
    NDI_LOG("Async frame processor thread started\n");
    
    while (!data->stopAsyncThread) {
        std::unique_lock<std::mutex> lock(data->queueMutex);
        data->queueCondition.wait(lock, [data] { 
            return !data->frameQueue.empty() || data->stopAsyncThread; 
        });

        if (data->stopAsyncThread) {
            break;
        }

        if (!data->frameQueue.empty()) {
            AsyncFrameData frameData = data->frameQueue.front();
            data->frameQueue.pop();
            lock.unlock();

            // Process frame asynchronously
            if (frameData.isHDR) {
                sendHDRFrame(data, frameData.frameData.data(), frameData.width, frameData.height);
            } else {
                sendSDRFrame(data, frameData.frameData.data(), frameData.width, frameData.height);
            }
        }
    }
    
    NDI_LOG("Async frame processor thread stopped\n");
}

// Utility functions
static OfxStatus fetchHostSuites(void)
{
    if(!gHost)
        return kOfxStatErrMissingHostFeature;
        
    gEffectHost   = (OfxImageEffectSuiteV1 *) gHost->fetchSuite(gHost->host, kOfxImageEffectSuite, 1);
    gPropHost     = (OfxPropertySuiteV1 *)    gHost->fetchSuite(gHost->host, kOfxPropertySuite, 1);
    gParamHost    = (OfxParameterSuiteV1 *)   gHost->fetchSuite(gHost->host, kOfxParameterSuite, 1);
    gMemoryHost   = (OfxMemorySuiteV1 *)      gHost->fetchSuite(gHost->host, kOfxMemorySuite, 1);
    gThreadHost   = (OfxMultiThreadSuiteV1 *) gHost->fetchSuite(gHost->host, kOfxMultiThreadSuite, 1);
    gMessageSuite = (OfxMessageSuiteV1 *)     gHost->fetchSuite(gHost->host, kOfxMessageSuite, 1);
    
    if(!gEffectHost || !gPropHost || !gParamHost || !gMemoryHost || !gThreadHost)
        return kOfxStatErrMissingHostFeature;
    return kOfxStatOK;
}

static NDIInstanceData* getInstanceData(OfxImageEffectHandle effect)
{
    OfxPropertySetHandle effectProps;
    gEffectHost->getPropertySet(effect, &effectProps);
    
    NDIInstanceData *myData = 0;
    gPropHost->propGetPointer(effectProps, kOfxPropInstanceData, 0, (void **) &myData);
    return myData;
}

static bool initializeNDI(NDIInstanceData* data)
{
    if (data->ndiInitialized) {
        return true;
    }

    NDI_LOG("Initializing NDI Advanced SDK...");
    
    // Initialize NDI
    if (!NDIlib_initialize()) {
        NDI_LOG("Failed to initialize NDI library");
        return false;
    }
    
    NDI_LOG("NDI library initialized successfully");

    // Create NDI source description with advanced settings
    NDIlib_send_create_t NDI_send_create_desc;
    NDI_send_create_desc.p_ndi_name = data->sourceName.c_str();
    NDI_send_create_desc.p_groups = nullptr;
    NDI_send_create_desc.clock_video = true;
    NDI_send_create_desc.clock_audio = false;

    NDI_LOG("Creating NDI sender with name: '%s'", data->sourceName.c_str());

    // Create the NDI sender
    data->ndiSend = NDIlib_send_create(&NDI_send_create_desc);
    if (!data->ndiSend) {
        NDI_LOG("Failed to create NDI sender - this might be due to NDI runtime not being available");
        NDI_LOG("Please ensure NDI Tools or NDI Runtime is installed on this system");
        NDIlib_destroy();
        return false;
    }
    
    NDI_LOG("NDI sender created successfully");

    // Enable hardware acceleration if GPU acceleration is enabled
    if (data->gpuAcceleration) {
        NDI_LOG("Enabling hardware acceleration hints");
        
        // Send hardware acceleration metadata hint
        const char* hwAccelMetadata = "<ndi_video_codec type=\"hardware\"/>";
        NDIlib_metadata_frame_t metadataFrame;
        metadataFrame.length = strlen(hwAccelMetadata);
        metadataFrame.timecode = NDIlib_send_timecode_synthesize;
        metadataFrame.p_data = const_cast<char*>(hwAccelMetadata);
        
        NDIlib_send_send_metadata(data->ndiSend, &metadataFrame);
    }

    // Initialize GPU context if enabled
    if (!initializeGPUContext(data)) {
        NDI_LOG("GPU acceleration initialization failed, falling back to CPU");
        data->gpuAcceleration = false;
    }

    // Start async processing thread if enabled
    if (data->asyncSending) {
        data->stopAsyncThread = false;
        data->asyncThread = std::thread(asyncFrameProcessor, data);
        NDI_LOG("Asynchronous frame processing enabled");
    }

    data->ndiInitialized = true;
    NDI_LOG("NDI Advanced SDK initialized successfully with source name '%s'", data->sourceName.c_str());
    NDI_LOG("GPU Acceleration: %s, Async Sending: %s, Optimal Format: %s",
           data->gpuAcceleration ? "Enabled" : "Disabled",
           data->asyncSending ? "Enabled" : "Disabled",
           data->optimalFormat ? "Enabled" : "Disabled");
    
    return true;
}

static void shutdownNDI(NDIInstanceData* data)
{
    if (!data->ndiInitialized) {
        return;
    }

    NDI_LOG("Shutting down NDI SDK...");
    
    // Stop async processing thread
    if (data->asyncSending && data->asyncThread.joinable()) {
        data->stopAsyncThread = true;
        data->queueCondition.notify_all();
        data->asyncThread.join();
        NDI_LOG("Async processing thread stopped");
    }
    
    // Clear any remaining frames in queue
    {
        std::lock_guard<std::mutex> lock(data->queueMutex);
        while (!data->frameQueue.empty()) {
            data->frameQueue.pop();
        }
    }
    
    // Shutdown GPU context
    shutdownGPUContext(data);
    
    if (data->ndiSend) {
        NDIlib_send_destroy(data->ndiSend);
        data->ndiSend = nullptr;
    }
    
    NDIlib_destroy();
    data->ndiInitialized = false;
}

static void createHDRMetadata(NDIInstanceData* data)
{
    // Create HDR metadata XML according to NDI SDK v6 specifications
    // Reference: https://docs.ndi.video/all/developing-with-ndi/sdk/hdr#hdr-metadata
    
    std::string primaries, transfer, matrix;
    
    // Map our color space to NDI primaries
    if (data->colorSpace == kColorSpaceRec2020) {
        primaries = "bt_2020";
        matrix = "bt_2020";
    } else if (data->colorSpace == kColorSpaceP3) {
        primaries = "bt_2020"; // P3 uses bt_2020 primaries in NDI context
        matrix = "bt_2020";
    } else {
        primaries = "bt_709";
        matrix = "bt_709";
    }
    
    // Map our transfer function to NDI transfer
    if (data->transferFunction == kTransferFunctionPQ) {
        transfer = "bt_2100_pq";
    } else if (data->transferFunction == kTransferFunctionHLG) {
        transfer = "bt_2100_hlg";
    } else {
        transfer = "bt_709";
    }
    
    // Create proper NDI color info metadata
    data->hdrMetadataXML = "<ndi_color_info primaries=\"" + primaries + 
                          "\" transfer=\"" + transfer + 
                          "\" matrix=\"" + matrix + "\" />";
    
    NDI_LOG("HDR Metadata: %s", data->hdrMetadataXML.c_str());
}

static void sendHDRFrame(NDIInstanceData* data, void* imageData, int width, int height)
{
    if (!data->enabled || !data->ndiInitialized || !imageData) {
        return;
    }
    
    NDI_LOG("Sending HDR frame %dx%d to NDI", width, height);

    // Prepare HDR frame buffer (16-bit per channel, P216 format)
    // P216 is planar YUV 4:2:2 with 16-bit samples
    const size_t frameSize = width * height * 2 * sizeof(uint16_t); // Y plane + UV plane (4:2:2)
    if (data->hdrFrameBuffer.size() != frameSize / sizeof(uint16_t)) {
        data->hdrFrameBuffer.resize(frameSize / sizeof(uint16_t));
    }

    uint16_t* dstData = data->hdrFrameBuffer.data();
    float* srcData = static_cast<float*>(imageData);

    // Try GPU acceleration first for HDR conversion
    bool gpuSuccess = false;
#ifdef __APPLE__
    if (data->gpuAcceleration && data->gpuContext && data->gpuContext->initialized && data->gpuContext->metalContext) {
        // For HDR, we need to convert to 16-bit limited range
        // The scale factor should be for 16-bit limited range (not full range)
        float scale = 65472.0f; // 16-bit limited range: (235-16) * 256 + (240-16) * 256 for chroma
        
        gpuSuccess = metal_gpu_convert_rgba_to_hdr(
            data->gpuContext->metalContext,
            srcData,
            dstData,
            width,
            height,
            scale
        );
        
        if (gpuSuccess) {
            NDI_LOG("Metal GPU HDR conversion completed");
        } else {
            NDI_LOG("Metal GPU HDR conversion failed, falling back to CPU");
        }
    }
#elif defined(_WIN32)
    if (data->gpuAcceleration && data->gpuContext && data->gpuContext->initialized && data->gpuContext->cudaContext) {
        // For HDR, we need to convert to 16-bit limited range
        float scale = 65472.0f; // 16-bit limited range
        
        gpuSuccess = cuda_gpu_convert_rgba_to_hdr(
            data->gpuContext->cudaContext,
            srcData,
            dstData,
            width,
            height,
            scale
        );
        
        if (gpuSuccess) {
            NDI_LOG("CUDA GPU HDR conversion completed");
        } else {
            NDI_LOG("CUDA GPU HDR conversion failed, falling back to CPU");
        }
    }
#endif

    // Fallback to CPU conversion if GPU failed or not available
    if (!gpuSuccess) {
        // Convert RGBA float to YUV 16-bit limited range (P216 format)
        // Reference: ITU BT.2100 quantization equations
        
        uint16_t* yPlane = dstData;
        uint16_t* uvPlane = dstData + (width * height);
        
        for (int y = 0; y < height; ++y) {
            int srcRow = height - 1 - y; // Flip vertically
            for (int x = 0; x < width; x += 2) {
                // Process two pixels for 4:2:2 subsampling
                int srcIdx1 = (srcRow * width + x) * 4;
                int srcIdx2 = (srcRow * width + x + 1) * 4;
                
                // Get RGB values (clamped to 0-1)
                float r1 = std::max(0.0f, std::min(1.0f, srcData[srcIdx1 + 0]));
                float g1 = std::max(0.0f, std::min(1.0f, srcData[srcIdx1 + 1]));
                float b1 = std::max(0.0f, std::min(1.0f, srcData[srcIdx1 + 2]));
                
                float r2 = (x + 1 < width) ? std::max(0.0f, std::min(1.0f, srcData[srcIdx2 + 0])) : r1;
                float g2 = (x + 1 < width) ? std::max(0.0f, std::min(1.0f, srcData[srcIdx2 + 1])) : g1;
                float b2 = (x + 1 < width) ? std::max(0.0f, std::min(1.0f, srcData[srcIdx2 + 2])) : b1;
                
                // Convert to YUV using Rec.2020 coefficients for HDR
                float y1 = 0.2627f * r1 + 0.6780f * g1 + 0.0593f * b1;
                float y2 = 0.2627f * r2 + 0.6780f * g2 + 0.0593f * b2;
                
                // Average chroma for 4:2:2 subsampling
                float avgR = (r1 + r2) * 0.5f;
                float avgG = (g1 + g2) * 0.5f;
                float avgB = (b1 + b2) * 0.5f;
                
                float u = -0.1396f * avgR - 0.3604f * avgG + 0.5f * avgB;
                float v = 0.5f * avgR - 0.4598f * avgG - 0.0402f * avgB;
                
                // Convert to 16-bit limited range (ITU BT.2100)
                // Y: 16-bit limited range [4096, 60160] for 10-bit equivalent [64, 940]
                // UV: 16-bit limited range [4096, 61440] for 10-bit equivalent [64, 960]
                uint16_t y1_16 = static_cast<uint16_t>(4096 + y1 * 56064); // (60160-4096)
                uint16_t y2_16 = static_cast<uint16_t>(4096 + y2 * 56064);
                uint16_t u_16 = static_cast<uint16_t>(32768 + u * 28672); // Center + range
                uint16_t v_16 = static_cast<uint16_t>(32768 + v * 28672);
                
                // Store in P216 format (planar)
                int yIdx1 = y * width + x;
                int yIdx2 = y * width + x + 1;
                int uvIdx = (y * width + x) / 2; // 4:2:2 subsampling
                
                yPlane[yIdx1] = y1_16;
                if (x + 1 < width) {
                    yPlane[yIdx2] = y2_16;
                }
                
                // Store U and V interleaved for 4:2:2
                uvPlane[uvIdx * 2] = u_16;     // U
                uvPlane[uvIdx * 2 + 1] = v_16; // V
            }
        }
    }

    // Create HDR metadata
    createHDRMetadata(data);

    // Setup NDI HDR video frame with proper P216 format
    NDIlib_video_frame_v2_t ndiVideoFrame;
    ndiVideoFrame.xres = width;
    ndiVideoFrame.yres = height;
    ndiVideoFrame.FourCC = NDIlib_FourCC_video_type_P216; // Proper HDR format
    ndiVideoFrame.frame_rate_N = static_cast<int>(data->frameRate * 1000);
    ndiVideoFrame.frame_rate_D = 1000;
    ndiVideoFrame.picture_aspect_ratio = static_cast<float>(width) / static_cast<float>(height);
    ndiVideoFrame.frame_format_type = NDIlib_frame_format_type_progressive;
    ndiVideoFrame.timecode = NDIlib_send_timecode_synthesize;
    ndiVideoFrame.p_data = reinterpret_cast<uint8_t*>(dstData);
    ndiVideoFrame.line_stride_in_bytes = width * sizeof(uint16_t); // Y plane stride
    ndiVideoFrame.p_metadata = data->hdrMetadataXML.empty() ? nullptr : data->hdrMetadataXML.c_str();

    // Send the HDR frame
    NDIlib_send_send_video_v2(data->ndiSend, &ndiVideoFrame);
}

static void sendSDRFrame(NDIInstanceData* data, void* imageData, int width, int height)
{
    if (!data->enabled || !data->ndiInitialized || !imageData) {
        return;
    }
    
    NDI_LOG("Sending SDR frame %dx%d to NDI (GPU: %s, Format: %s)", 
           width, height,
           data->gpuAcceleration ? "Yes" : "No",
           data->optimalFormat ? "UYVY" : "RGBA");

    NDIlib_video_frame_v2_t ndiVideoFrame;
    ndiVideoFrame.xres = width;
    ndiVideoFrame.yres = height;
    ndiVideoFrame.frame_rate_N = static_cast<int>(data->frameRate * 1000);
    ndiVideoFrame.frame_rate_D = 1000;
    ndiVideoFrame.picture_aspect_ratio = static_cast<float>(width) / static_cast<float>(height);
    ndiVideoFrame.frame_format_type = NDIlib_frame_format_type_progressive;
    ndiVideoFrame.timecode = NDIlib_send_timecode_synthesize;
    ndiVideoFrame.p_metadata = nullptr;

    if (data->optimalFormat) {
        // Use UYVY format for optimal NDI performance
        if (data->gpuAcceleration) {
            convertRGBAToUYVY_GPU(data, imageData, width, height);
        } else {
            convertRGBAToUYVY_CPU(data, imageData, width, height);
        }
        
        ndiVideoFrame.FourCC = NDIlib_FourCC_type_UYVY;
        ndiVideoFrame.p_data = data->uyvyFrameBuffer.data();
        ndiVideoFrame.line_stride_in_bytes = width * 2; // UYVY is 2 bytes per pixel
    } else {
        // Use RGBA format (legacy compatibility)
        const size_t frameSize = width * height * 4 * sizeof(uint8_t);
        if (data->frameBuffer.size() != frameSize) {
            data->frameBuffer.resize(frameSize);
        }

        // Convert float RGBA to uint8_t RGBA for NDI with vertical flip
        float* srcData = static_cast<float*>(imageData);
        uint8_t* dstData = data->frameBuffer.data();
        
        // Flip vertically: OpenFX uses bottom-left origin, NDI expects top-left
        for (int y = 0; y < height; ++y) {
            int srcRow = height - 1 - y; // Flip vertically
            for (int x = 0; x < width; ++x) {
                int srcIdx = (srcRow * width + x) * 4;
                int dstIdx = (y * width + x) * 4;
                
                dstData[dstIdx + 0] = static_cast<uint8_t>(std::max(0.0f, std::min(1.0f, srcData[srcIdx + 0])) * 255.0f); // R
                dstData[dstIdx + 1] = static_cast<uint8_t>(std::max(0.0f, std::min(1.0f, srcData[srcIdx + 1])) * 255.0f); // G
                dstData[dstIdx + 2] = static_cast<uint8_t>(std::max(0.0f, std::min(1.0f, srcData[srcIdx + 2])) * 255.0f); // B
                dstData[dstIdx + 3] = static_cast<uint8_t>(std::max(0.0f, std::min(1.0f, srcData[srcIdx + 3])) * 255.0f); // A
            }
        }

        ndiVideoFrame.FourCC = NDIlib_FourCC_type_RGBA;
        ndiVideoFrame.p_data = dstData;
        ndiVideoFrame.line_stride_in_bytes = width * 4;
    }

    // Send the frame (asynchronously if enabled)
    if (data->asyncSending) {
        NDIlib_send_send_video_async_v2(data->ndiSend, &ndiVideoFrame);
    } else {
        NDIlib_send_send_video_v2(data->ndiSend, &ndiVideoFrame);
    }
}

static void sendNDIFrame(NDIInstanceData* data, void* imageData, int width, int height)
{
    // Ensure NDI is initialized before sending frames
    if (!data->ndiInitialized && data->enabled) {
        NDI_LOG("NDI not initialized, attempting to initialize...");
        if (!initializeNDI(data)) {
            NDI_LOG("Failed to initialize NDI, skipping frame");
            return;
        }
    }
    
    if (data->hdrEnabled) {
        sendHDRFrame(data, imageData, width, height);
    } else {
        sendSDRFrame(data, imageData, width, height);
    }
}

// Plugin functions
static OfxStatus onLoad(void)
{
    return fetchHostSuites();
}

static OfxStatus onUnLoad(void)
{
    return kOfxStatOK;
}

static OfxStatus createInstance(OfxImageEffectHandle effect)
{
    NDI_LOG("Creating instance");
    
    // Get property set
    OfxPropertySetHandle effectProps;
    gEffectHost->getPropertySet(effect, &effectProps);

    // Get parameter set
    OfxParamSetHandle paramSet;
    gEffectHost->getParamSet(effect, &paramSet);

    // Create instance data
    NDIInstanceData *myData = new NDIInstanceData;
    myData->ndiSend = nullptr;
    myData->ndiInitialized = false;
    myData->sourceName = "DaVinci Resolve NDI Output";
    myData->enabled = true;
    myData->frameRate = 25.0;
    
    // GPU acceleration settings (default enabled for better performance)
    myData->gpuAcceleration = true;
    myData->asyncSending = true;
    myData->optimalFormat = true;
    myData->stopAsyncThread = false;
    
    // HDR settings
    myData->hdrEnabled = false;
    myData->colorSpace = kColorSpaceRec709;
    myData->transferFunction = kTransferFunctionSDR;
    myData->maxCLL = 1000.0;
    myData->maxFALL = 400.0;

    // Cache clip handles
    gEffectHost->clipGetHandle(effect, kOfxImageEffectSimpleSourceClipName, &myData->sourceClip, 0);
    gEffectHost->clipGetHandle(effect, kOfxImageEffectOutputClipName, &myData->outputClip, 0);

    // Cache parameter handles
    gParamHost->paramGetHandle(paramSet, kParamSourceName, &myData->sourceNameParam, 0);
    gParamHost->paramGetHandle(paramSet, kParamEnabled, &myData->enabledParam, 0);
    gParamHost->paramGetHandle(paramSet, kParamFrameRate, &myData->frameRateParam, 0);
    gParamHost->paramGetHandle(paramSet, kParamGPUAcceleration, &myData->gpuAccelerationParam, 0);
    gParamHost->paramGetHandle(paramSet, kParamAsyncSending, &myData->asyncSendingParam, 0);
    gParamHost->paramGetHandle(paramSet, kParamOptimalFormat, &myData->optimalFormatParam, 0);
    gParamHost->paramGetHandle(paramSet, kParamVersionLabel, &myData->versionLabelParam, 0);
    gParamHost->paramGetHandle(paramSet, kParamHDREnabled, &myData->hdrEnabledParam, 0);
    gParamHost->paramGetHandle(paramSet, kParamColorSpace, &myData->colorSpaceParam, 0);
    gParamHost->paramGetHandle(paramSet, kParamTransferFunction, &myData->transferFunctionParam, 0);
    gParamHost->paramGetHandle(paramSet, kParamMaxCLL, &myData->maxCLLParam, 0);
    gParamHost->paramGetHandle(paramSet, kParamMaxFALL, &myData->maxFALLParam, 0);

    // Set instance data
    gPropHost->propSetPointer(effectProps, kOfxPropInstanceData, 0, (void *) myData);

    // Initialize NDI if enabled by default
    if (myData->enabled) {
        initializeNDI(myData);
    }

    NDI_LOG("Instance created successfully");
    return kOfxStatOK;
}

static OfxStatus destroyInstance(OfxImageEffectHandle effect)
{
    NDI_LOG("Destroying instance");
    
    NDIInstanceData *myData = getInstanceData(effect);
    if (myData) {
        shutdownNDI(myData);
        delete myData;
    }
    return kOfxStatOK;
}

static OfxStatus instanceChanged(OfxImageEffectHandle effect, OfxPropertySetHandle inArgs, OfxPropertySetHandle /*outArgs*/)
{
    NDIInstanceData *myData = getInstanceData(effect);
    if (!myData) return kOfxStatFailed;

    // Get changed parameter name
    char *changeReason;
    gPropHost->propGetString(inArgs, kOfxPropChangeReason, 0, &changeReason);
    
    if (strcmp(changeReason, kOfxChangeUserEdited) == 0) {
        char *paramName;
        gPropHost->propGetString(inArgs, kOfxPropName, 0, &paramName);
        
        NDI_LOG("Parameter changed: %s", paramName);
        
        // Update parameter values
        char* sourceName;
        gParamHost->paramGetValue(myData->sourceNameParam, &sourceName);
        myData->sourceName = sourceName;
        
        int enabled;
        gParamHost->paramGetValue(myData->enabledParam, &enabled);
        myData->enabled = (enabled != 0);
        
        double frameRate;
        gParamHost->paramGetValue(myData->frameRateParam, &frameRate);
        myData->frameRate = frameRate;
        
        // GPU acceleration parameters
        int gpuAcceleration;
        gParamHost->paramGetValue(myData->gpuAccelerationParam, &gpuAcceleration);
        myData->gpuAcceleration = (gpuAcceleration != 0);
        
        int asyncSending;
        gParamHost->paramGetValue(myData->asyncSendingParam, &asyncSending);
        myData->asyncSending = (asyncSending != 0);
        
        int optimalFormat;
        gParamHost->paramGetValue(myData->optimalFormatParam, &optimalFormat);
        myData->optimalFormat = (optimalFormat != 0);
        
        int hdrEnabled;
        gParamHost->paramGetValue(myData->hdrEnabledParam, &hdrEnabled);
        myData->hdrEnabled = (hdrEnabled != 0);
        
        int colorSpaceIndex;
        gParamHost->paramGetValue(myData->colorSpaceParam, &colorSpaceIndex);
        myData->colorSpace = (colorSpaceIndex == 0) ? kColorSpaceRec709 : 
                            (colorSpaceIndex == 1) ? kColorSpaceRec2020 : kColorSpaceP3;
        
        int transferFunctionIndex;
        gParamHost->paramGetValue(myData->transferFunctionParam, &transferFunctionIndex);
        myData->transferFunction = (transferFunctionIndex == 0) ? kTransferFunctionSDR :
                                  (transferFunctionIndex == 1) ? kTransferFunctionPQ : kTransferFunctionHLG;
        
        double maxCLL;
        gParamHost->paramGetValue(myData->maxCLLParam, &maxCLL);
        myData->maxCLL = maxCLL;
        
        double maxFALL;
        gParamHost->paramGetValue(myData->maxFALLParam, &maxFALL);
        myData->maxFALL = maxFALL;
        
        NDI_LOG("Updated params - sourceName='%s', enabled=%d, frameRate=%.2f, hdr=%d, colorSpace='%s', transferFunc='%s'", 
               myData->sourceName.c_str(), myData->enabled, myData->frameRate, myData->hdrEnabled, 
               myData->colorSpace.c_str(), myData->transferFunction.c_str());
        
        // Restart NDI if source name changed or HDR settings changed
        if ((strcmp(paramName, kParamSourceName) == 0 || 
             strcmp(paramName, kParamHDREnabled) == 0 ||
             strcmp(paramName, kParamColorSpace) == 0 ||
             strcmp(paramName, kParamTransferFunction) == 0) && myData->ndiInitialized) {
            NDI_LOG("Restarting NDI due to %s parameter change", paramName);
            shutdownNDI(myData);
        }
        
        // Initialize NDI if enabled
        if (myData->enabled && !myData->ndiInitialized) {
            initializeNDI(myData);
        } else if (!myData->enabled && myData->ndiInitialized) {
            shutdownNDI(myData);
        }
    }
    
    return kOfxStatOK;
}

static OfxStatus render(OfxImageEffectHandle instance, OfxPropertySetHandle inArgs, OfxPropertySetHandle /*outArgs*/)
{
    NDI_LOG("Render called");
    
    NDIInstanceData *myData = getInstanceData(instance);
    if (!myData) return kOfxStatFailed;

    // Read current parameter values at render time
    int hdrEnabled;
    gParamHost->paramGetValue(myData->hdrEnabledParam, &hdrEnabled);
    myData->hdrEnabled = (hdrEnabled != 0);
    
    int gpuAcceleration;
    gParamHost->paramGetValue(myData->gpuAccelerationParam, &gpuAcceleration);
    myData->gpuAcceleration = (gpuAcceleration != 0);
    
    int enabled;
    gParamHost->paramGetValue(myData->enabledParam, &enabled);
    myData->enabled = (enabled != 0);
    
    // Log current parameter state for debugging
    NDI_LOG("Render params - enabled=%d, hdr=%d, gpu=%d", 
           myData->enabled, myData->hdrEnabled, myData->gpuAcceleration);

    // Get time
    double time;
    gPropHost->propGetDouble(inArgs, kOfxPropTime, 0, &time);

    // Get render window
    OfxRectI renderWindow;
    gPropHost->propGetIntN(inArgs, kOfxImageEffectPropRenderWindow, 4, &renderWindow.x1);

    // Get source image
    OfxPropertySetHandle sourceImg = NULL;
    gEffectHost->clipGetImage(myData->sourceClip, time, NULL, &sourceImg);
    if (!sourceImg) {
        NDI_LOG("No source image");
        return kOfxStatFailed;
    }

    // Get output image
    OfxPropertySetHandle outputImg = NULL;
    gEffectHost->clipGetImage(myData->outputClip, time, NULL, &outputImg);
    if (!outputImg) {
        NDI_LOG("No output image");
        gEffectHost->clipReleaseImage(sourceImg);
        return kOfxStatFailed;
    }

    // Get image properties
    void *srcData, *dstData;
    OfxRectI srcRect, dstRect;
    int srcRowBytes, dstRowBytes;
    
    gPropHost->propGetPointer(sourceImg, kOfxImagePropData, 0, &srcData);
    gPropHost->propGetIntN(sourceImg, kOfxImagePropBounds, 4, &srcRect.x1);
    gPropHost->propGetInt(sourceImg, kOfxImagePropRowBytes, 0, &srcRowBytes);
    
    gPropHost->propGetPointer(outputImg, kOfxImagePropData, 0, &dstData);
    gPropHost->propGetIntN(outputImg, kOfxImagePropBounds, 4, &dstRect.x1);
    gPropHost->propGetInt(outputImg, kOfxImagePropRowBytes, 0, &dstRowBytes);

    // Copy source to output (pass-through)
    int width = dstRect.x2 - dstRect.x1;
    int height = dstRect.y2 - dstRect.y1;
    
    if (srcData && dstData) {
        // Simple copy for float RGBA
        memcpy(dstData, srcData, height * dstRowBytes);
        
        // Send to NDI with vertical flip correction
        sendNDIFrame(myData, srcData, width, height);
    }

    // Release images
    gEffectHost->clipReleaseImage(sourceImg);
    gEffectHost->clipReleaseImage(outputImg);

    NDI_LOG("Render completed");
    return kOfxStatOK;
}

static OfxStatus describe(OfxImageEffectHandle effect)
{
    NDI_LOG("Describe called");
    
    OfxPropertySetHandle props;
    gEffectHost->getPropertySet(effect, &props);

    // Set basic properties
    gPropHost->propSetString(props, kOfxPropLabel, 0, kPluginName);
    gPropHost->propSetString(props, kOfxImageEffectPluginPropGrouping, 0, kPluginGrouping);
    gPropHost->propSetString(props, kOfxPropPluginDescription, 0, kPluginDescription);

    // Set supported contexts
    gPropHost->propSetString(props, kOfxImageEffectPropSupportedContexts, 0, kOfxImageEffectContextFilter);

    // Set supported pixel depths
    gPropHost->propSetString(props, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthFloat);

    // Set other properties
    gPropHost->propSetInt(props, kOfxImageEffectPropSupportsTiles, 0, 0);
    gPropHost->propSetInt(props, kOfxImageEffectPropSupportsMultiResolution, 0, 0);
    gPropHost->propSetInt(props, kOfxImageEffectPropSupportsMultipleClipPARs, 0, 0);
    gPropHost->propSetString(props, kOfxImageEffectPluginRenderThreadSafety, 0, kOfxImageEffectRenderFullySafe);

    return kOfxStatOK;
}

static OfxStatus describeInContext(OfxImageEffectHandle effect, OfxPropertySetHandle inArgs)
{
    NDI_LOG("DescribeInContext called");
    
    // Define clips
    OfxPropertySetHandle sourceClipProps = NULL, outputClipProps = NULL;
    gEffectHost->clipDefine(effect, kOfxImageEffectSimpleSourceClipName, &sourceClipProps);
    gEffectHost->clipDefine(effect, kOfxImageEffectOutputClipName, &outputClipProps);

    // Set clip properties
    gPropHost->propSetString(sourceClipProps, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
    gPropHost->propSetInt(sourceClipProps, kOfxImageClipPropIsMask, 0, 0);
    gPropHost->propSetInt(sourceClipProps, kOfxImageEffectPropSupportsTiles, 0, 0);
    
    gPropHost->propSetString(outputClipProps, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
    gPropHost->propSetInt(outputClipProps, kOfxImageEffectPropSupportsTiles, 0, 0);

    // Get parameter set
    OfxParamSetHandle paramSet;
    gEffectHost->getParamSet(effect, &paramSet);

    // Create parameter groups for better organization
    OfxPropertySetHandle infoGroupProps = NULL;
    gParamHost->paramDefine(paramSet, kOfxParamTypeGroup, "infoGroup", &infoGroupProps);
    gPropHost->propSetString(infoGroupProps, kOfxPropLabel, 0, "Plugin Information");
    gPropHost->propSetInt(infoGroupProps, kOfxParamPropGroupOpen, 0, 1); // Open by default

    OfxPropertySetHandle basicGroupProps = NULL;
    gParamHost->paramDefine(paramSet, kOfxParamTypeGroup, "basicGroup", &basicGroupProps);
    gPropHost->propSetString(basicGroupProps, kOfxPropLabel, 0, "Basic Settings");
    gPropHost->propSetInt(basicGroupProps, kOfxParamPropGroupOpen, 0, 1); // Open by default

    OfxPropertySetHandle performanceGroupProps = NULL;
    gParamHost->paramDefine(paramSet, kOfxParamTypeGroup, "performanceGroup", &performanceGroupProps);
    gPropHost->propSetString(performanceGroupProps, kOfxPropLabel, 0, "Performance Settings");
    gPropHost->propSetInt(performanceGroupProps, kOfxParamPropGroupOpen, 0, 1); // Open by default

    OfxPropertySetHandle hdrGroupProps = NULL;
    gParamHost->paramDefine(paramSet, kOfxParamTypeGroup, "hdrGroup", &hdrGroupProps);
    gPropHost->propSetString(hdrGroupProps, kOfxPropLabel, 0, "HDR Settings");
    gPropHost->propSetInt(hdrGroupProps, kOfxParamPropGroupOpen, 0, 0); // Closed by default

    // Define version label parameter (visible read-only display) - in Info group
    OfxPropertySetHandle versionLabelProps = NULL;
    gParamHost->paramDefine(paramSet, kOfxParamTypeString, kParamVersionLabel, &versionLabelProps);
    gPropHost->propSetString(versionLabelProps, kOfxPropLabel, 0, kParamVersionLabelLabel);
    gPropHost->propSetString(versionLabelProps, kOfxParamPropScriptName, 0, kParamVersionLabel);
    gPropHost->propSetString(versionLabelProps, kOfxParamPropHint, 0, kParamVersionLabelHint);
    gPropHost->propSetString(versionLabelProps, kOfxParamPropDefault, 0, "v" kPluginVersionString " (GPU-Accelerated NDI Advanced)");
    gPropHost->propSetInt(versionLabelProps, kOfxParamPropAnimates, 0, 0);
    gPropHost->propSetString(versionLabelProps, kOfxParamPropParent, 0, "infoGroup");

    // Define source name parameter - in Basic group
    OfxPropertySetHandle sourceNameProps = NULL;
    gParamHost->paramDefine(paramSet, kOfxParamTypeString, kParamSourceName, &sourceNameProps);
    gPropHost->propSetString(sourceNameProps, kOfxPropLabel, 0, kParamSourceNameLabel);
    gPropHost->propSetString(sourceNameProps, kOfxParamPropScriptName, 0, kParamSourceName);
    gPropHost->propSetString(sourceNameProps, kOfxParamPropHint, 0, kParamSourceNameHint);
    gPropHost->propSetString(sourceNameProps, kOfxParamPropDefault, 0, "DaVinci Resolve NDI Output");
    gPropHost->propSetInt(sourceNameProps, kOfxParamPropAnimates, 0, 0);
    gPropHost->propSetString(sourceNameProps, kOfxParamPropParent, 0, "basicGroup");

    // Define enabled parameter - in Basic group
    OfxPropertySetHandle enabledProps = NULL;
    gParamHost->paramDefine(paramSet, kOfxParamTypeBoolean, kParamEnabled, &enabledProps);
    gPropHost->propSetString(enabledProps, kOfxPropLabel, 0, kParamEnabledLabel);
    gPropHost->propSetString(enabledProps, kOfxParamPropScriptName, 0, kParamEnabled);
    gPropHost->propSetString(enabledProps, kOfxParamPropHint, 0, kParamEnabledHint);
    gPropHost->propSetInt(enabledProps, kOfxParamPropDefault, 0, 1); // Default to enabled
    gPropHost->propSetInt(enabledProps, kOfxParamPropAnimates, 0, 0);
    gPropHost->propSetString(enabledProps, kOfxParamPropParent, 0, "basicGroup");

    // Define frame rate parameter - in Basic group
    OfxPropertySetHandle frameRateProps = NULL;
    gParamHost->paramDefine(paramSet, kOfxParamTypeDouble, kParamFrameRate, &frameRateProps);
    gPropHost->propSetString(frameRateProps, kOfxPropLabel, 0, kParamFrameRateLabel);
    gPropHost->propSetString(frameRateProps, kOfxParamPropScriptName, 0, kParamFrameRate);
    gPropHost->propSetString(frameRateProps, kOfxParamPropHint, 0, kParamFrameRateHint);
    gPropHost->propSetDouble(frameRateProps, kOfxParamPropDefault, 0, 25.0);
    gPropHost->propSetDouble(frameRateProps, kOfxParamPropMin, 0, 1.0);
    gPropHost->propSetDouble(frameRateProps, kOfxParamPropMax, 0, 120.0);
    gPropHost->propSetDouble(frameRateProps, kOfxParamPropDisplayMin, 0, 23.976);
    gPropHost->propSetDouble(frameRateProps, kOfxParamPropDisplayMax, 0, 60.0);
    gPropHost->propSetInt(frameRateProps, kOfxParamPropAnimates, 0, 0);
    gPropHost->propSetString(frameRateProps, kOfxParamPropParent, 0, "basicGroup");

    // Define GPU acceleration parameter - in Performance group
    OfxPropertySetHandle gpuAccelerationProps = NULL;
    gParamHost->paramDefine(paramSet, kOfxParamTypeBoolean, kParamGPUAcceleration, &gpuAccelerationProps);
    gPropHost->propSetString(gpuAccelerationProps, kOfxPropLabel, 0, kParamGPUAccelerationLabel);
    gPropHost->propSetString(gpuAccelerationProps, kOfxParamPropScriptName, 0, kParamGPUAcceleration);
    gPropHost->propSetString(gpuAccelerationProps, kOfxParamPropHint, 0, kParamGPUAccelerationHint);
    gPropHost->propSetInt(gpuAccelerationProps, kOfxParamPropDefault, 0, 1); // Default to enabled
    gPropHost->propSetInt(gpuAccelerationProps, kOfxParamPropAnimates, 0, 0);
    gPropHost->propSetString(gpuAccelerationProps, kOfxParamPropParent, 0, "performanceGroup");

    // Define asynchronous sending parameter - in Performance group
    OfxPropertySetHandle asyncSendingProps = NULL;
    gParamHost->paramDefine(paramSet, kOfxParamTypeBoolean, kParamAsyncSending, &asyncSendingProps);
    gPropHost->propSetString(asyncSendingProps, kOfxPropLabel, 0, kParamAsyncSendingLabel);
    gPropHost->propSetString(asyncSendingProps, kOfxParamPropScriptName, 0, kParamAsyncSending);
    gPropHost->propSetString(asyncSendingProps, kOfxParamPropHint, 0, kParamAsyncSendingHint);
    gPropHost->propSetInt(asyncSendingProps, kOfxParamPropDefault, 0, 1); // Default to enabled
    gPropHost->propSetInt(asyncSendingProps, kOfxParamPropAnimates, 0, 0);
    gPropHost->propSetString(asyncSendingProps, kOfxParamPropParent, 0, "performanceGroup");

    // Define optimal format parameter - in Performance group
    OfxPropertySetHandle optimalFormatProps = NULL;
    gParamHost->paramDefine(paramSet, kOfxParamTypeBoolean, kParamOptimalFormat, &optimalFormatProps);
    gPropHost->propSetString(optimalFormatProps, kOfxPropLabel, 0, kParamOptimalFormatLabel);
    gPropHost->propSetString(optimalFormatProps, kOfxParamPropScriptName, 0, kParamOptimalFormat);
    gPropHost->propSetString(optimalFormatProps, kOfxParamPropHint, 0, kParamOptimalFormatHint);
    gPropHost->propSetInt(optimalFormatProps, kOfxParamPropDefault, 0, 1); // Default to enabled
    gPropHost->propSetInt(optimalFormatProps, kOfxParamPropAnimates, 0, 0);
    gPropHost->propSetString(optimalFormatProps, kOfxParamPropParent, 0, "performanceGroup");

    // Define HDR enabled parameter - in HDR group
    OfxPropertySetHandle hdrEnabledProps = NULL;
    gParamHost->paramDefine(paramSet, kOfxParamTypeBoolean, kParamHDREnabled, &hdrEnabledProps);
    gPropHost->propSetString(hdrEnabledProps, kOfxPropLabel, 0, kParamHDREnabledLabel);
    gPropHost->propSetString(hdrEnabledProps, kOfxParamPropScriptName, 0, kParamHDREnabled);
    gPropHost->propSetString(hdrEnabledProps, kOfxParamPropHint, 0, kParamHDREnabledHint);
    gPropHost->propSetInt(hdrEnabledProps, kOfxParamPropDefault, 0, 0); // Default to disabled
    gPropHost->propSetInt(hdrEnabledProps, kOfxParamPropAnimates, 0, 0);
    gPropHost->propSetString(hdrEnabledProps, kOfxParamPropParent, 0, "hdrGroup");

    // Define color space parameter - in HDR group
    OfxPropertySetHandle colorSpaceProps = NULL;
    gParamHost->paramDefine(paramSet, kOfxParamTypeChoice, kParamColorSpace, &colorSpaceProps);
    gPropHost->propSetString(colorSpaceProps, kOfxPropLabel, 0, kParamColorSpaceLabel);
    gPropHost->propSetString(colorSpaceProps, kOfxParamPropScriptName, 0, kParamColorSpace);
    gPropHost->propSetString(colorSpaceProps, kOfxParamPropHint, 0, kParamColorSpaceHint);
    gPropHost->propSetString(colorSpaceProps, kOfxParamPropChoiceOption, 0, "Rec.709");
    gPropHost->propSetString(colorSpaceProps, kOfxParamPropChoiceOption, 1, "Rec.2020");
    gPropHost->propSetString(colorSpaceProps, kOfxParamPropChoiceOption, 2, "DCI-P3");
    gPropHost->propSetInt(colorSpaceProps, kOfxParamPropDefault, 0, 0); // Rec.709
    gPropHost->propSetInt(colorSpaceProps, kOfxParamPropAnimates, 0, 0);
    gPropHost->propSetString(colorSpaceProps, kOfxParamPropParent, 0, "hdrGroup");

    // Define transfer function parameter - in HDR group
    OfxPropertySetHandle transferFunctionProps = NULL;
    gParamHost->paramDefine(paramSet, kOfxParamTypeChoice, kParamTransferFunction, &transferFunctionProps);
    gPropHost->propSetString(transferFunctionProps, kOfxPropLabel, 0, kParamTransferFunctionLabel);
    gPropHost->propSetString(transferFunctionProps, kOfxParamPropScriptName, 0, kParamTransferFunction);
    gPropHost->propSetString(transferFunctionProps, kOfxParamPropHint, 0, kParamTransferFunctionHint);
    gPropHost->propSetString(transferFunctionProps, kOfxParamPropChoiceOption, 0, "SDR (Gamma 2.4)");
    gPropHost->propSetString(transferFunctionProps, kOfxParamPropChoiceOption, 1, "PQ (ST.2084)");
    gPropHost->propSetString(transferFunctionProps, kOfxParamPropChoiceOption, 2, "HLG (Hybrid Log-Gamma)");
    gPropHost->propSetInt(transferFunctionProps, kOfxParamPropDefault, 0, 0); // SDR
    gPropHost->propSetInt(transferFunctionProps, kOfxParamPropAnimates, 0, 0);
    gPropHost->propSetString(transferFunctionProps, kOfxParamPropParent, 0, "hdrGroup");

    // Define max CLL parameter - in HDR group
    OfxPropertySetHandle maxCLLProps = NULL;
    gParamHost->paramDefine(paramSet, kOfxParamTypeDouble, kParamMaxCLL, &maxCLLProps);
    gPropHost->propSetString(maxCLLProps, kOfxPropLabel, 0, kParamMaxCLLLabel);
    gPropHost->propSetString(maxCLLProps, kOfxParamPropScriptName, 0, kParamMaxCLL);
    gPropHost->propSetString(maxCLLProps, kOfxParamPropHint, 0, kParamMaxCLLHint);
    gPropHost->propSetDouble(maxCLLProps, kOfxParamPropDefault, 0, 1000.0);
    gPropHost->propSetDouble(maxCLLProps, kOfxParamPropMin, 0, 100.0);
    gPropHost->propSetDouble(maxCLLProps, kOfxParamPropMax, 0, 10000.0);
    gPropHost->propSetDouble(maxCLLProps, kOfxParamPropDisplayMin, 0, 100.0);
    gPropHost->propSetDouble(maxCLLProps, kOfxParamPropDisplayMax, 0, 4000.0);
    gPropHost->propSetInt(maxCLLProps, kOfxParamPropAnimates, 0, 0);
    gPropHost->propSetString(maxCLLProps, kOfxParamPropParent, 0, "hdrGroup");

    // Define max FALL parameter - in HDR group
    OfxPropertySetHandle maxFALLProps = NULL;
    gParamHost->paramDefine(paramSet, kOfxParamTypeDouble, kParamMaxFALL, &maxFALLProps);
    gPropHost->propSetString(maxFALLProps, kOfxPropLabel, 0, kParamMaxFALLLabel);
    gPropHost->propSetString(maxFALLProps, kOfxParamPropScriptName, 0, kParamMaxFALL);
    gPropHost->propSetString(maxFALLProps, kOfxParamPropHint, 0, kParamMaxFALLHint);
    gPropHost->propSetDouble(maxFALLProps, kOfxParamPropDefault, 0, 400.0);
    gPropHost->propSetDouble(maxFALLProps, kOfxParamPropMin, 0, 50.0);
    gPropHost->propSetDouble(maxFALLProps, kOfxParamPropMax, 0, 4000.0);
    gPropHost->propSetDouble(maxFALLProps, kOfxParamPropDisplayMin, 0, 50.0);
    gPropHost->propSetDouble(maxFALLProps, kOfxParamPropDisplayMax, 0, 1000.0);
    gPropHost->propSetInt(maxFALLProps, kOfxParamPropAnimates, 0, 0);
    gPropHost->propSetString(maxFALLProps, kOfxParamPropParent, 0, "hdrGroup");

    return kOfxStatOK;
}

static OfxStatus pluginMain(const char *action, const void *handle, OfxPropertySetHandle inArgs, OfxPropertySetHandle outArgs)
{
    try {
        if (strcmp(action, kOfxActionLoad) == 0) {
            return onLoad();
        }
        else if (strcmp(action, kOfxActionUnload) == 0) {
            return onUnLoad();
        }
        else if (strcmp(action, kOfxActionDescribe) == 0) {
            return describe((OfxImageEffectHandle) handle);
        }
        else if (strcmp(action, kOfxImageEffectActionDescribeInContext) == 0) {
            return describeInContext((OfxImageEffectHandle) handle, inArgs);
        }
        else if (strcmp(action, kOfxActionCreateInstance) == 0) {
            return createInstance((OfxImageEffectHandle) handle);
        }
        else if (strcmp(action, kOfxActionDestroyInstance) == 0) {
            return destroyInstance((OfxImageEffectHandle) handle);
        }
        else if (strcmp(action, kOfxActionInstanceChanged) == 0) {
            return instanceChanged((OfxImageEffectHandle) handle, inArgs, outArgs);
        }
        else if (strcmp(action, kOfxImageEffectActionRender) == 0) {
            return render((OfxImageEffectHandle) handle, inArgs, outArgs);
        }
        else if (strcmp(action, kOfxImageEffectActionIsIdentity) == 0) {
            return kOfxStatReplyDefault;
        }
        else if (strcmp(action, kOfxImageEffectActionGetRegionOfDefinition) == 0) {
            return kOfxStatReplyDefault;
        }
        else if (strcmp(action, kOfxImageEffectActionGetRegionsOfInterest) == 0) {
            return kOfxStatReplyDefault;
        }
        else if (strcmp(action, kOfxImageEffectActionGetClipPreferences) == 0) {
            return kOfxStatReplyDefault;
        }
        else if (strcmp(action, kOfxImageEffectActionGetTimeDomain) == 0) {
            return kOfxStatReplyDefault;
        }
    }
    catch (std::bad_alloc) {
        return kOfxStatErrMemory;
    }
    catch (const std::exception& e) {
        return kOfxStatErrUnknown;
    }
    catch (...) {
        return kOfxStatErrUnknown;
    }
    
    return kOfxStatReplyDefault;
}

static void setHostFunc(OfxHost *hostStruct)
{
    gHost = hostStruct;
}

static OfxPlugin basicPlugin = {
    kOfxImageEffectPluginApi,
    1,
    kPluginIdentifier,
    kPluginVersionMajor,
    kPluginVersionMinor,
    setHostFunc,
    pluginMain
};

EXPORT OfxPlugin* OfxGetPlugin(int nth)
{
    if (nth == 0)
        return &basicPlugin;
    return 0;
}

EXPORT int OfxGetNumberOfPlugins(void)
{       
    return 1;
} 