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
#include <map>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <chrono>

#ifdef __APPLE__
#include <os/log.h>
#define NDI_LOG(fmt, ...) os_log(OS_LOG_DEFAULT, "NDI Plugin: " fmt, ##__VA_ARGS__)
// For dynamic strings a human must be able to read in `log stream`/`log show`:
// os_log can redact plain %s arguments as <private> depending on system config.
#define NDI_LOG_TEXT(str) os_log(OS_LOG_DEFAULT, "NDI Plugin: %{public}s", str)
#elif defined(_WIN32)
// Windows sink: OutputDebugStringA for live capture (DebugView/WinDbg), plus
// an append-only file sink when NDI_OUTPUT_LOG_FILE names a path. Resolve on
// Windows has no console, so printf would go nowhere.
static void ndiWinLog(const char* fmt, ...);
#define NDI_LOG(fmt, ...) ndiWinLog("NDI Plugin: " fmt, ##__VA_ARGS__)
#define NDI_LOG_TEXT(str) ndiWinLog("NDI Plugin: %s", str)
#else
#define NDI_LOG(fmt, ...) printf("NDI Plugin: " fmt "\n", ##__VA_ARGS__)
#define NDI_LOG_TEXT(str) printf("NDI Plugin: %s\n", str)
#endif

#include "ofxImageEffect.h"
#include "ofxImageEffectExt.h"
#include "ofxMemory.h"
#include "ofxMultiThread.h"

// Resolve's CUDA-stream extension properties (ticket #22). Present in the
// official ofxGPURender.h; the vendored ofxImageEffectExt.h predates them.
#ifndef kOfxImageEffectPropCudaStreamSupported
#define kOfxImageEffectPropCudaStreamSupported "OfxImageEffectPropCudaStreamSupported"
#endif
#ifndef kOfxImageEffectPropCudaStream
#define kOfxImageEffectPropCudaStream "OfxImageEffectPropCudaStream"
#endif

#include <algorithm>

#include "BRAWLensMap.h"
#include "NDIRuntimeLoader.h"
#include "PlatformPaths.h"
#include "RenderProbe.h"
#include "STMap.h"
#include "StereoPair.h"
#include "StreamResolution.h"

#ifdef __APPLE__
#include "BRAWImmersiveReader.h"
#include "MacFileDialog.h"
#include "MetalGPUAcceleration.h"
#include "TimelineClipWatcher.h"
#endif

#ifdef NDI_HAS_CUDA // defined by CMake when the CUDA module is in the build
#include "CudaGPUAcceleration.h"
#endif

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdarg>
#include <cstdlib>
#include "WinFileDialog.h"
#endif

// Native browse-dialog seam (ticket #24): both desktop platforms pop a
// native open dialog from the browse push-buttons, behind one contract
// (UTF-8 in/out; cancel and every failure return false with the buffer
// untouched). Aliased like the GPU modules so the param plumbing below
// compiles identically on both.
#if defined(__APPLE__)
#define NDI_HAS_BROWSE_DIALOGS 1
#define native_open_file_dialog mac_open_file_dialog
#elif defined(_WIN32)
#define NDI_HAS_BROWSE_DIALOGS 1
#define native_open_file_dialog win_open_file_dialog
#endif

// NDI Advanced SDK
#include <Processing.NDI.Lib.h>

#if defined __APPLE__ || defined __linux__ || defined __FreeBSD__
#  define EXPORT __attribute__((visibility("default")))
#elif defined _WIN32
#  define EXPORT OfxExport
#else
#  error Not building on your operating system quite yet
#endif

#ifdef _WIN32
// NDI_LOG sink for Windows. Live capture: run DebugView (Sysinternals) or
// attach WinDbg — OutputDebugStringA lines carry the "NDI Plugin: " prefix.
// Optional file sink: set NDI_OUTPUT_LOG_FILE to a writable path before
// launching Resolve and lines are appended there too (fwrite on one FILE* is
// thread-safe in the MSVC CRT; render threads log concurrently).
static void ndiWinLog(const char* fmt, ...)
{
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf(buf, sizeof(buf) - 2, fmt, args);
    va_end(args);
    if (n < 0) {
        return;
    }
    size_t len = static_cast<size_t>(n);
    if (len > sizeof(buf) - 3) {
        // vsnprintf truncated: it wrote sizeof(buf)-3 chars plus its own NUL
        // at [sizeof(buf)-3]; the newline must overwrite that NUL, not land
        // after it (an embedded NUL would cut the line short in both sinks).
        len = sizeof(buf) - 3;
    }
    buf[len] = '\n';
    buf[len + 1] = '\0';
    OutputDebugStringA(buf);

    static std::FILE* sink = []() -> std::FILE* {
        const char* path = std::getenv("NDI_OUTPUT_LOG_FILE");
        return (path && *path) ? ndi_path::fopenUtf8(path, "ab") : nullptr;
    }();
    if (sink) {
        std::fwrite(buf, 1, len + 1, sink);
        std::fflush(sink);
    }
}
#endif // _WIN32

// Plugin constants
#define kPluginName "NDIOutput"
#define kPluginGrouping "LSVR"
#define kPluginDescription \
"NDI Advanced Output v" kPluginVersionString " (GPU-Accelerated): GPU-accelerated NDI streaming with HDR support. \n" \
"Configure the NDI source name, HDR settings, GPU acceleration, and enable/disable the output stream. \n" \
"Version: " kPluginVersionString " - GPU-Accelerated NDI Advanced"
#define kPluginIdentifier "LSVR.NDIOutput"
#define kPluginVersionMajor 1
#define kPluginVersionMinor 14
#define kPluginVersionPatch 0
#define kPluginVersionString "1.14.0"

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

#define kParamResolution "resolution"
#define kParamResolutionLabel "Resolution"
#define kParamResolutionHint "NDI stream resolution as a fraction of the incoming frame: Full, Half, or Quarter. When the host provides GPU frames, the downscale runs on the GPU before any readback."

// Stereo Parameters (issue #6)
#define kParamStereoPacking "stereoPacking"
#define kParamStereoPackingLabel "Stereo Packing"
#define kParamStereoPackingHint "How a stereo pair is arranged in the single outgoing NDI frame when the timeline renders both eyes (Stereo 3D palette at Vision: Stereo): Side-by-Side (left eye left) or Top-Bottom (left eye top). Mono timelines are unaffected."

#define kParamStereoStatus "stereoStatus"
#define kParamStereoStatusLabel "Stream Status"
#define kParamStereoStatusHint "What the stream is currently carrying: Mono, a packed stereo pair, a labeled single-eye fallback when the partner eye stops rendering, or the sender-creation failure (e.g. the NDI name is already in use on this machine)."

// Projection Parameters (issue #7)
#define kParamProjection "projectionMode"
#define kParamProjectionLabel "Projection"
#define kParamProjectionHint "Projection of the outgoing stream. Passthrough sends the timeline's native projection untouched. Equirect (STMap) warps each eye through the loaded STMap EXR(s) before packing, so a fisheye/lens-space timeline (e.g. Apple Immersive) displays correctly on equirect receivers such as the Quest stereo-180 player; the warped output resolution equals the STMap's resolution. Equirect (Camera Metadata) does the same warp but derives the per-eye maps from the lens calibration embedded in URSA Cine Immersive BRAW clips — by default following whichever clip is under the playhead (see Camera Clip Source), so multi-camera timelines just work; no STMap files needed. The output is a 180ºx180º equirect per eye at the Metadata Map Size. Either way the Resolution control applies on top, and a missing/invalid map source falls back to Passthrough and says so in Stream Status."

#define kParamSTMapLayout "stmapLayout"
#define kParamSTMapLayoutLabel "STMap Layout"
#define kParamSTMapLayoutHint "How the STMap file(s) carry the eyes. Packed Side-by-Side (default): the STMap field holds ONE EXR whose left half maps the left eye and right half the right eye (Canon VR-style authoring), for Stereo 3D timelines; each half's U convention (per-eye vs packed-frame coordinates) is auto-detected and logged. Per-Eye Files: separate left/right fields appear and each loaded EXR applies whole to each rendered frame — also the right choice for a packed-frame source on a mono timeline (e.g. a dual-fisheye clip: one map in the left slot warps the whole packed frame). Eye assignment always comes from the timeline's stereo tracks — the map halves define geometry only."

#define kParamSTMapPacked "stmapPacked"
#define kParamSTMapPackedLabel "STMap"
#define kParamSTMapPackedHint "Packed side-by-side STMap: ONE 32-bit float (or half) EXR whose left half maps the left eye and right half the right eye. R = normalized source U, G = normalized source V, bottom-left origin (Fusion/Nuke convention). Scanline EXR with None/RLE/Zip compression. Required for Equirect (STMap) mode in this layout."

#define kParamSTMapLeft "stmapLeft"
#define kParamSTMapLeftLabel "STMap (Left Eye)"
#define kParamSTMapLeftHint "32-bit float (or half) EXR STMap for the left eye: R = normalized source U, G = normalized source V, bottom-left origin (Fusion/Nuke convention). Scanline EXR with None/RLE/Zip compression. Required for Equirect mode; also used for the right eye when no right-eye map is set, and for mono timelines."

#define kParamSTMapRight "stmapRight"
#define kParamSTMapRightLabel "STMap (Right Eye)"
#define kParamSTMapRightHint "Optional right-eye STMap EXR (same format and resolution as the left map). Leave empty to warp both eyes through the left map."

// Native browse buttons: Resolve renders filePath string params as plain
// text fields with no browse control (verified 2026-08-30), so the plugin
// pops its own native open dialog from push-button params — NSOpenPanel on
// macOS, IFileOpenDialog on Windows (the browse-dialog seam above).
#define kParamSTMapPackedBrowse "stmapPackedBrowse"
#define kParamSTMapPackedBrowseLabel "Browse for STMap..."
#define kParamSTMapPackedBrowseHint "Pick the packed side-by-side STMap EXR with a file dialog and fill the path field above."
#define kParamSTMapLeftBrowse "stmapLeftBrowse"
#define kParamSTMapLeftBrowseLabel "Browse for Left-Eye STMap..."
#define kParamSTMapLeftBrowseHint "Pick the left-eye STMap EXR with a file dialog and fill the path field above."
#define kParamSTMapRightBrowse "stmapRightBrowse"
#define kParamSTMapRightBrowseLabel "Browse for Right-Eye STMap..."
#define kParamSTMapRightBrowseHint "Pick the right-eye STMap EXR with a file dialog and fill the path field above."

// Camera-metadata projection (issue #11): per-eye warp maps generated from the
// Apple Immersive lens calibration embedded in URSA Cine Immersive BRAW.
#define kParamBRAWSource "brawClipSource"
#define kParamBRAWSourceLabel "Camera Clip Source"
#define kParamBRAWSourceHint "Where Equirect (Camera Metadata) gets its calibration. Timeline (Auto) follows the clip under the playhead — multi-camera timelines pick up each camera's own calibration as the playhead crosses it (maps are cached per camera, and gaps/non-BRAW clips keep the last camera's maps rather than popping to passthrough). Auto needs Resolve's external scripting enabled (Preferences > System > General > External scripting using: Local) and a system python3; if either is missing the log says so — use Manual Path, which reads the clip picked below instead."
#define kParamBRAWClip "brawClip"
#define kParamBRAWClipLabel "Camera Clip (.braw)"
#define kParamBRAWClipHint "Manual Path mode only: any URSA Cine Immersive BRAW clip shot on the camera — the embedded factory calibration is per-camera, not per-shot, so one pick covers every clip from the same body. The clip is opened for its metadata (instant), never decoded."
#define kParamBRAWClipBrowse "brawClipBrowse"
#define kParamBRAWClipBrowseLabel "Browse for Camera Clip..."
#define kParamBRAWClipBrowseHint "Pick the camera's BRAW clip with a file dialog and fill the path field above."
#define kParamBRAWMapSize "metadataMapSize"
#define kParamBRAWMapSizeLabel "Metadata Map Size"
#define kParamBRAWMapSizeHint "Resolution of the generated per-eye 180ºx180º equirect (Camera Metadata mode only) — like an STMap's resolution, this is the warped output size per eye before the Resolution control divides it. 2048 is right for monitoring; 4096 costs 4x the map memory."
#define kParamBRAWMask "metadataMask"
#define kParamBRAWMaskLabel "Metadata Edge Mask"
#define kParamBRAWMaskHint "Off streams the full calibrated 180º field. Camera applies the clip's embedded visionOS-style porthole (a circle ~80º off-axis on this lens), hiding the smeary outer lens edge the way Apple players do — currently a hard cut at the circle, not the 2.5º feather."

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

// Diagnostics Parameters
#define kParamDebugLogging "debugLogging"
#define kParamDebugLoggingLabel "Log Render Calls"
#define kParamDebugLoggingHint "Log one 'NDI Plugin: probe' line per render call (page, eye, thumbnail flag, frame time, dimensions, inter-call spacing) to the system log. Capture with scripts/capture_probe_log.sh. Leave off unless diagnosing host behavior."

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

// ---------------------------------------------------------------------------
// GPU-native seam (spec decision 5): one header-level C API, two
// implementations — Metal on macOS, CUDA on Windows (ticket #22). These
// aliases let everything above the seam (the async pump, the render fast
// path, the STMap upload cache) compile identically on both platforms; a
// platform with neither module runs the CPU path.
// ---------------------------------------------------------------------------
#if defined(__APPLE__) || defined(NDI_HAS_CUDA)
#define NDI_GPU_NATIVE 1
#endif

#ifdef __APPLE__
#define NDI_GPU_BACKEND_NAME "Metal"
typedef MetalGPUContextRef NativeGPUContextRef;
typedef metal_submit_status NativeSubmitStatus;
typedef metal_downscale_done_fn native_gpu_done_fn;
static const NativeSubmitStatus kNativeSubmitOK = METAL_SUBMIT_OK;
static const NativeSubmitStatus kNativeSubmitBusy = METAL_SUBMIT_BUSY;
static const NativeSubmitStatus kNativeSubmitInvalid = METAL_SUBMIT_INVALID;
static inline bool nativeGpuAvailable(void) { return metal_gpu_is_available(); }
static inline NativeGPUContextRef nativeGpuInit(void) { return metal_gpu_init(); }
static inline void nativeGpuShutdown(NativeGPUContextRef c) { metal_gpu_shutdown(c); }
static inline bool nativeGpuCopyBuffer(NativeGPUContextRef c, void* q, void* src, void* dst,
                                       size_t bytes, bool wait)
{ return metal_gpu_copy_buffer(c, q, src, dst, bytes, wait); }
static inline bool nativeGpuReadBuffer(NativeGPUContextRef c, void* q, void* src,
                                       void* cpuDst, size_t bytes)
{ return metal_gpu_read_buffer(c, q, src, cpuDst, bytes); }
static inline NativeSubmitStatus nativeGpuDownscaleSubmit(NativeGPUContextRef c, void* q, void* src,
                                                          int sw, int sh, int srf, int divisor,
                                                          int ow, int oh, bool p216,
                                                          native_gpu_done_fn done, void* user)
{ return metal_gpu_downscale_submit(c, q, src, sw, sh, srf, divisor, ow, oh, p216, done, user); }
static inline NativeSubmitStatus nativeGpuWarpSubmit(NativeGPUContextRef c, void* q, void* src,
                                                     int sw, int sh, int srf,
                                                     void* map, int mw, int mh, int divisor,
                                                     int ow, int oh, bool p216,
                                                     native_gpu_done_fn done, void* user)
{ return metal_gpu_warp_submit(c, q, src, sw, sh, srf, map, mw, mh, divisor, ow, oh, p216, done, user); }
static inline void nativeGpuDownscaleRelease(NativeGPUContextRef c, void* slot)
{ metal_gpu_downscale_release(c, slot); }
static inline void* nativeGpuCreateBufferForQueue(NativeGPUContextRef c, void* q,
                                                  const void* data, size_t bytes)
{ return metal_gpu_create_shared_buffer_for_queue(c, q, data, bytes); }
static inline void* nativeGpuQueueDevice(NativeGPUContextRef c, void* q)
{ return metal_gpu_queue_device(c, q); }
static inline void nativeGpuReleaseBuffer(void* buffer) { metal_gpu_release_buffer(buffer); }
#elif defined(NDI_HAS_CUDA)
#define NDI_GPU_BACKEND_NAME "CUDA"
typedef CudaGPUContextRef NativeGPUContextRef;
typedef cuda_submit_status NativeSubmitStatus;
typedef cuda_downscale_done_fn native_gpu_done_fn;
static const NativeSubmitStatus kNativeSubmitOK = CUDA_SUBMIT_OK;
static const NativeSubmitStatus kNativeSubmitBusy = CUDA_SUBMIT_BUSY;
static const NativeSubmitStatus kNativeSubmitInvalid = CUDA_SUBMIT_INVALID;
static inline bool nativeGpuAvailable(void) { return cuda_gpu_is_available(); }
static inline NativeGPUContextRef nativeGpuInit(void) { return cuda_gpu_init(); }
static inline void nativeGpuShutdown(NativeGPUContextRef c) { cuda_gpu_shutdown(c); }
static inline bool nativeGpuCopyBuffer(NativeGPUContextRef c, void* q, void* src, void* dst,
                                       size_t bytes, bool wait)
{ return cuda_gpu_copy_buffer(c, q, src, dst, bytes, wait); }
static inline bool nativeGpuReadBuffer(NativeGPUContextRef c, void* q, void* src,
                                       void* cpuDst, size_t bytes)
{ return cuda_gpu_read_buffer(c, q, src, cpuDst, bytes); }
static inline NativeSubmitStatus nativeGpuDownscaleSubmit(NativeGPUContextRef c, void* q, void* src,
                                                          int sw, int sh, int srf, int divisor,
                                                          int ow, int oh, bool p216,
                                                          native_gpu_done_fn done, void* user)
{ return cuda_gpu_downscale_submit(c, q, src, sw, sh, srf, divisor, ow, oh, p216, done, user); }
static inline NativeSubmitStatus nativeGpuWarpSubmit(NativeGPUContextRef c, void* q, void* src,
                                                     int sw, int sh, int srf,
                                                     void* map, int mw, int mh, int divisor,
                                                     int ow, int oh, bool p216,
                                                     native_gpu_done_fn done, void* user)
{ return cuda_gpu_warp_submit(c, q, src, sw, sh, srf, map, mw, mh, divisor, ow, oh, p216, done, user); }
static inline void nativeGpuDownscaleRelease(NativeGPUContextRef c, void* slot)
{ cuda_gpu_downscale_release(c, slot); }
static inline void* nativeGpuCreateBufferForQueue(NativeGPUContextRef c, void* q,
                                                  const void* data, size_t bytes)
{ return cuda_gpu_create_device_buffer_for_stream(c, q, data, bytes); }
static inline void* nativeGpuQueueDevice(NativeGPUContextRef c, void* q)
{ return cuda_gpu_stream_device(c, q); }
static inline void nativeGpuReleaseBuffer(void* buffer) { cuda_gpu_release_buffer(buffer); }
#endif

// GPU Processing Context: the platform GPU-module context behind the seam
// above. On a platform with neither module the CPU path renders.
struct GPUContext {
#ifdef NDI_GPU_NATIVE
    NativeGPUContextRef nativeContext;
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

struct SenderHub; // process-shared NDI sender + eye pairer (defined below)
#ifdef NDI_GPU_NATIVE
struct AsyncPump; // per-instance off-render-thread NDI worker (defined below)
#endif

// ---------------------------------------------------------------------------
// STMap store (issue #7). One loaded map is shared process-wide via a
// weak-pointer cache: in stereo, both per-eye instances name the same files,
// and an 8K float map runs to hundreds of MB — loading it once matters. An
// entry is immutable after load (the per-device GPU upload cache is the one
// lazily-filled part, behind its own mutex); instances hold shared_ptrs and
// the map frees itself when the last instance lets go.
// ---------------------------------------------------------------------------

struct StmapEntry {
    std::string path;
    long long fileMtime = 0;
    long long fileSize = 0;
    bool valid = false;
    std::string error;
    ndi_stmap::STMapImage map;
#ifdef NDI_GPU_NATIVE
    // Per-device GPU upload of map.uv, created lazily on the render path and
    // reused every frame. Releasing while a frame is in flight is safe on
    // both backends: Metal command buffers retain the MTLBuffer, and CUDA's
    // cudaFree orders itself behind already-launched work.
    std::mutex gpuUploadMutex;
    std::map<void*, void*> gpuBufferByDevice; // key: device ptr; value: device buffer
    bool gpuUploadFailed = false;             // don't retry (or re-log) an OOM every frame
    ~StmapEntry()
    {
        for (auto& kv : gpuBufferByDevice) {
            nativeGpuReleaseBuffer(kv.second);
        }
    }
#endif
};

static std::mutex gStmapCacheMutex;
static std::map<std::string, std::weak_ptr<StmapEntry>> gStmapCache;

static bool stmapFileStat(const char* path, long long* mtime, long long* size)
{
    return ndi_path::statUtf8(path, mtime, size);
}

// Fetch the STMap at `path`, loading it only when no instance already holds a
// current copy (keyed on mtime+size, so overwriting the file and touching any
// parameter picks up the new bytes). The load itself runs OUTSIDE every lock —
// a large EXR takes real time and render threads must never queue behind it;
// they keep streaming passthrough until the caller swaps the entry in.
static std::shared_ptr<StmapEntry> stmapAcquire(const std::string& path)
{
    long long mtime = 0, size = 0;
    const bool statOk = stmapFileStat(path.c_str(), &mtime, &size);
    {
        std::lock_guard<std::mutex> lock(gStmapCacheMutex);
        for (auto it = gStmapCache.begin(); it != gStmapCache.end();) {
            it = it->second.expired() ? gStmapCache.erase(it) : std::next(it);
        }
        auto it = gStmapCache.find(path);
        if (it != gStmapCache.end()) {
            std::shared_ptr<StmapEntry> hit = it->second.lock();
            if (hit && statOk && hit->fileMtime == mtime && hit->fileSize == size) {
                return hit;
            }
            // Stale (file replaced) or vanished: fall through to a fresh load,
            // which surfaces the current state of the path.
        }
    }

    std::shared_ptr<StmapEntry> entry = std::make_shared<StmapEntry>();
    entry->path = path;
    entry->fileMtime = mtime;
    entry->fileSize = size;
    entry->valid = ndi_stmap::loadSTMapEXR(path.c_str(), &entry->map, &entry->error);
    if (entry->valid) {
        NDI_LOG_TEXT(("STMap loaded: '" + path + "' (" + std::to_string(entry->map.width) +
                      "x" + std::to_string(entry->map.height) + ")").c_str());
    } else {
        NDI_LOG_TEXT(("STMap load failed: '" + path + "' — " + entry->error).c_str());
    }

    std::lock_guard<std::mutex> lock(gStmapCacheMutex);
    gStmapCache[path] = entry;
    return entry;
}

// Fetch BOTH per-eye maps derived from one side-by-side packed STMap file
// (Canon VR-style authoring — see ndi_stmap::splitPackedSTMap for the split
// and U-convention auto-detection). The derived halves are cached process-
// wide under pseudo-keys (a real path can't contain '\n', so no collision
// with file entries) and staleness-checked against the SOURCE file like any
// entry; the full packed image is only held while splitting — once both
// halves exist, its cache slot expires and the memory frees.
static void stmapAcquirePackedPair(const std::string& path,
                                   std::shared_ptr<StmapEntry>* leftOut,
                                   std::shared_ptr<StmapEntry>* rightOut)
{
    const std::string keyLeft = path + "\n#packedSbS:L";
    const std::string keyRight = path + "\n#packedSbS:R";
    long long mtime = 0, size = 0;
    const bool statOk = stmapFileStat(path.c_str(), &mtime, &size);
    {
        std::lock_guard<std::mutex> lock(gStmapCacheMutex);
        auto itL = gStmapCache.find(keyLeft);
        auto itR = gStmapCache.find(keyRight);
        if (itL != gStmapCache.end() && itR != gStmapCache.end()) {
            std::shared_ptr<StmapEntry> left = itL->second.lock();
            std::shared_ptr<StmapEntry> right = itR->second.lock();
            if (left && right && statOk &&
                left->fileMtime == mtime && left->fileSize == size) {
                *leftOut = left;
                *rightOut = right;
                return;
            }
        }
    }

    std::shared_ptr<StmapEntry> packed = stmapAcquire(path); // shared, cached, stat-keyed
    std::shared_ptr<StmapEntry> left = std::make_shared<StmapEntry>();
    std::shared_ptr<StmapEntry> right = std::make_shared<StmapEntry>();
    left->path = keyLeft;
    right->path = keyRight;
    left->fileMtime = right->fileMtime = packed->fileMtime;
    left->fileSize = right->fileSize = packed->fileSize;
    if (!packed->valid) {
        left->error = right->error = packed->error;
    } else {
        ndi_stmap::PackedHalfCoords leftCoords, rightCoords;
        std::string splitError;
        if (ndi_stmap::splitPackedSTMap(packed->map, &left->map, &right->map,
                                        &leftCoords, &rightCoords, &splitError)) {
            left->valid = right->valid = true;
            NDI_LOG_TEXT(("STMap packed SbS split: '" + path + "' " +
                          std::to_string(packed->map.width) + "x" + std::to_string(packed->map.height) +
                          " -> 2x " + std::to_string(left->map.width) + "x" + std::to_string(left->map.height) +
                          "; left half: " + ndi_stmap::packedHalfCoordsName(leftCoords) +
                          "; right half: " + ndi_stmap::packedHalfCoordsName(rightCoords)).c_str());
        } else {
            left->error = right->error = splitError;
            NDI_LOG_TEXT(("STMap packed SbS split failed: '" + path + "' — " + splitError).c_str());
        }
    }

    std::lock_guard<std::mutex> lock(gStmapCacheMutex);
    gStmapCache[keyLeft] = left;
    gStmapCache[keyRight] = right;
    *leftOut = left;
    *rightOut = right;
}

// Fetch BOTH per-eye maps generated from an URSA Cine Immersive BRAW clip's
// embedded lens calibration (issue #11 — see src/BRAWLensMap.h for the
// geometry). Same derived-pseudo-key caching as the packed-SbS split above,
// staleness-checked against the .braw itself; a re-pick of the same clip (or
// both per-eye instances naming it) reuses the generated maps. The BRAW open
// is metadata-only and the generation ~30 ms at 2048² — both fine for the
// parameter-edit path this runs on.
static void brawAcquireLensPair(const std::string& path, int mapSize, bool applyMask,
                                std::shared_ptr<StmapEntry>* leftOut,
                                std::shared_ptr<StmapEntry>* rightOut)
{
    const std::string suffix = "\n#brawLens:" + std::to_string(mapSize) +
                               (applyMask ? ":mask" : ":nomask");
    const std::string keyLeft = path + suffix + ":L";
    const std::string keyRight = path + suffix + ":R";
    long long mtime = 0, size = 0;
    const bool statOk = stmapFileStat(path.c_str(), &mtime, &size);
    {
        std::lock_guard<std::mutex> lock(gStmapCacheMutex);
        auto itL = gStmapCache.find(keyLeft);
        auto itR = gStmapCache.find(keyRight);
        if (itL != gStmapCache.end() && itR != gStmapCache.end()) {
            std::shared_ptr<StmapEntry> left = itL->second.lock();
            std::shared_ptr<StmapEntry> right = itR->second.lock();
            if (left && right && statOk &&
                left->fileMtime == mtime && left->fileSize == size) {
                *leftOut = left;
                *rightOut = right;
                return;
            }
        }
    }

    std::shared_ptr<StmapEntry> left = std::make_shared<StmapEntry>();
    std::shared_ptr<StmapEntry> right = std::make_shared<StmapEntry>();
    left->path = keyLeft;
    right->path = keyRight;
    left->fileMtime = right->fileMtime = mtime;
    left->fileSize = right->fileSize = size;

#ifdef __APPLE__
    std::string json, kind, calType, error;
    ndi_brawmap::LensCalibration cal;
    if (!ndi_brawreader::readImmersiveCalibration(path, &json, &kind, &calType, &error) ||
        !ndi_brawmap::parseCalibrationJSON(json, &cal, &error)) {
        left->error = right->error = error;
        NDI_LOG_TEXT(("BRAW lens maps: '" + path + "' — " + error).c_str());
    } else {
        const bool maskUsable = cal.left.maskRadiusDeg > 0.0 && cal.right.maskRadiusDeg > 0.0;
        if (applyMask && !maskUsable) {
            NDI_LOG_TEXT(("BRAW lens maps: '" + path +
                          "' carries no usable maskData — edge mask skipped").c_str());
        }
        if (applyMask && cal.left.maskSpreadDeg > 1.0) {
            NDI_LOG("BRAW lens maps: mask ring is non-circular (%.1f deg spread) — "
                    "approximated by its mean circle",
                    cal.left.maskSpreadDeg);
        }
        if (!ndi_brawmap::generateLensMaps(cal, mapSize, applyMask, &left->map, &right->map,
                                           &error)) {
            left->error = right->error = error;
            NDI_LOG_TEXT(("BRAW lens maps: generation failed for '" + path + "' — " + error).c_str());
        } else {
            left->valid = right->valid = true;
            NDI_LOG_TEXT(("BRAW lens maps: '" + path + "' (" + kind + "/" + calType + ", " +
                          cal.generator + ") -> 2x " + std::to_string(mapSize) + "x" +
                          std::to_string(mapSize) + " equirect 180, mask " +
                          (applyMask && maskUsable
                               ? (std::to_string(cal.left.maskRadiusDeg) + " deg")
                               : std::string("off")))
                             .c_str());
        }
    }
#else
    (void)mapSize;
    (void)applyMask;
    left->error = right->error = "Camera-metadata projection is macOS-only for now";
#endif

    std::lock_guard<std::mutex> lock(gStmapCacheMutex);
    gStmapCache[keyLeft] = left;
    gStmapCache[keyRight] = right;
    *leftOut = left;
    *rightOut = right;
}

#ifdef NDI_GPU_NATIVE
// Per-device GPU upload of an STMap's uv data, cached on the shared entry:
// uploaded once, referenced by every subsequent frame's submission (safe if
// the entry dies mid-flight — see the cache comment above). Returns NULL on
// failure — callers fall back to the CPU warp. The upload itself (a
// multi-hundred-MB copy for a big map) runs OUTSIDE entry->gpuUploadMutex so
// the other eye's render thread never queues behind it; normally it doesn't
// run on a render thread at all — refreshSTMaps pre-warms it on the host's
// main thread whenever the context device matches the host queue's device
// (always, except multi-GPU machines).
static void* stmapNativeBufferForQueue(const std::shared_ptr<StmapEntry>& entry,
                                       NativeGPUContextRef nativeContext, void* gpuQueue)
{
    if (!entry || !entry->valid) {
        return nullptr;
    }
    void* deviceKey = nativeGpuQueueDevice(nativeContext, gpuQueue);
    if (!deviceKey) {
        return nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(entry->gpuUploadMutex);
        auto it = entry->gpuBufferByDevice.find(deviceKey);
        if (it != entry->gpuBufferByDevice.end()) {
            return it->second;
        }
        if (entry->gpuUploadFailed) {
            return nullptr;
        }
    }
    void* buffer = nativeGpuCreateBufferForQueue(nativeContext, gpuQueue,
                                                 entry->map.uv.data(),
                                                 entry->map.uv.size() * sizeof(float));
    std::lock_guard<std::mutex> lock(entry->gpuUploadMutex);
    if (!buffer) {
        entry->gpuUploadFailed = true; // don't retry (or re-log) an OOM every frame
        NDI_LOG("STMap: GPU upload failed (%zu bytes) — using the CPU warp fallback",
                entry->map.uv.size() * sizeof(float));
        return nullptr;
    }
    auto inserted = entry->gpuBufferByDevice.emplace(deviceKey, buffer);
    if (!inserted.second) {
        // Lost a concurrent upload race — keep the first, drop ours.
        nativeGpuReleaseBuffer(buffer);
        return inserted.first->second;
    }
    return buffer;
}
#endif // NDI_GPU_NATIVE

// Stream-status projection tag, composed into the Stream Status parameter.
enum ProjStatus {
    kProjOff = 0,       // Passthrough selected — no suffix
    kProjActive = 1,    // warping through valid maps
    kProjError = 2,     // Equirect selected but a map is missing/invalid: passthrough
    kProjMismatch = 3,  // L/R maps disagree on size (would split eye geometry): passthrough
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
    OfxParamHandle resolutionParam;
    OfxParamHandle gpuAccelerationParam;
    OfxParamHandle asyncSendingParam;
    OfxParamHandle optimalFormatParam;
    OfxParamHandle versionLabelParam;
    OfxParamHandle debugLoggingParam;
    OfxParamHandle stereoPackingParam;
    OfxParamHandle stereoStatusParam;
    OfxParamHandle projectionParam;
    OfxParamHandle stmapLayoutParam;
    OfxParamHandle stmapPackedParam;
    OfxParamHandle stmapLeftParam;
    OfxParamHandle stmapRightParam;
    OfxParamHandle brawSourceParam;
    OfxParamHandle brawClipParam;
#ifdef NDI_HAS_BROWSE_DIALOGS
    // Browse buttons (gated like their definitions) — cached so
    // updateParamVisibility can flip their secret state with the path fields.
    OfxParamHandle stmapPackedBrowseParam;
    OfxParamHandle stmapLeftBrowseParam;
    OfxParamHandle stmapRightBrowseParam;
    OfxParamHandle brawClipBrowseParam;
#endif
    OfxParamHandle brawMapSizeParam;
    OfxParamHandle brawMaskParam;
    OfxParamHandle hdrEnabledParam;
    OfxParamHandle colorSpaceParam;
    OfxParamHandle transferFunctionParam;
    OfxParamHandle maxCLLParam;
    OfxParamHandle maxFALLParam;

    // Diagnostic render-call probe
    std::string resolvePage;   // page that instantiated this effect (host provides it only at createInstance)
    std::mutex probeMutex;     // renders are declared fully thread-safe, so probe state needs its own guard
    long long probeCallCount;
    std::chrono::steady_clock::time_point probeLastCallTime;
    bool probeHasLastCallTime;

    // In-render stage timers ("mtimer" log line, issue #5 diagnosis). Written
    // only when Log Render Calls is on; fields are per-instance, and the hub
    // writes the submit splits into the SUBMITTING instance's fields.
    bool timersEnabled;
    double timerBlitMs, timerConvMs, timerFlushMs, timerPackMs, timerSendMs;

    // NDI variables. The sender lives in a process-shared hub — Resolve
    // renders each stereo eye through its own plugin instance, so senders and
    // pairing can never be instance state (see the SenderHub comment below).
    SenderHub* hub;
#ifdef NDI_GPU_NATIVE
    AsyncPump* pump;           // created on first async submit; drained in shutdownNDI
#endif
    bool ndiInitialized;
    std::string sourceName;
    bool enabled;
    double frameRate;

    // Per-render-call context for the stereo pairer, stashed by render()
    // exactly like resolutionDivisor (per-instance renders are serialized —
    // every conversion buffer below already relies on that).
    int renderEye;             // ndi_stereo::kEyeLeft / kEyeRight
    double renderTime;         // kOfxPropTime — the pairing key
    bool renderIsThumbnail;
    int stereoPacking;         // 0 = Side-by-Side, 1 = Top-Bottom

    // Projection normalization (issue #7). The loaded entries swap under
    // stmapMutex on parameter changes (main thread) while render threads copy
    // a shared_ptr per render, so a mid-flight swap can never free a map a
    // render is reading. renderStmap is this render's selected map (null =
    // passthrough), stashed like renderEye.
    int projectionMode = 0;             // 0 = Passthrough, 1 = Equirect (STMap), 2 = Equirect (Camera Metadata)
    int stmapLayout = 1;                // 0 = Per-Eye Files, 1 = Packed Side-by-Side (default)
    std::string stmapPackedPathWanted;  // parameter values as last read
    std::string stmapLeftPathWanted;
    std::string stmapRightPathWanted;
    std::string brawClipPathWanted;     // Camera Metadata mode's .braw (issue #11)
    int brawSourceChoice = 0;           // 0 = Timeline (Auto), 1 = Manual Path
    int brawMapSizeChoice = 1;          // 0 = 1024, 1 = 2048, 2 = 4096 (per eye)
    int brawMaskChoice = 0;             // 0 = Off, 1 = Camera porthole
    // Auto camera-clip mode: config mirror the playhead watcher's thread
    // reads (written by readInstanceParams on the main thread), plus the
    // last auto path applied/skipped — both under stmapMutex.
    std::atomic<bool> autoLensWanted{false};
    std::atomic<int> autoLensMapSize{2048};
    std::atomic<bool> autoLensMask{false};
    std::string autoLensAppliedPath;
    std::string autoLensSkippedPath;
    std::mutex stmapMutex;
    std::shared_ptr<StmapEntry> stmapLeft;
    std::shared_ptr<StmapEntry> stmapRight;   // null: left map serves both eyes
    std::atomic<int> projStatus{kProjOff};
    std::shared_ptr<StmapEntry> renderStmap;
    // Stream status handoff: the pump worker composes status off-thread while
    // render threads flush it to the UI param, so both fields live behind
    // their own small mutex (taken after hub->mutex, never the reverse).
    std::mutex statusMutex;
    bool statusParamDirty;     // stream status changed; push to the UI param
    std::string statusParamValue;

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
    
    // Stream resolution (issue #5): divisor 1/2/4, read fresh each render
    int resolutionDivisor;

    // Frame buffers
    std::vector<uint8_t> frameBuffer;
    std::vector<uint16_t> hdrFrameBuffer;
    std::vector<uint8_t> uyvyFrameBuffer; // UYVY format for optimal performance
    std::vector<float> downscaleBuffer;   // CPU-path box-downscale output
    std::vector<float> readbackBuffer;    // full-frame readback when a Metal frame needs the CPU path
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

// ---------------------------------------------------------------------------
// Process-shared NDI sender hub (issue #6).
//
// Why this exists (probe findings, docs/2026-08-28-render-call-probe-findings.md,
// and the LEARNINGS.md entries of 2026-08-28): with the Stereo 3D palette at
// Vision: Stereo, Resolve creates a SECOND plugin instance for the right eye.
// Per-instance senders then fight — both instances create a sender under the
// same source name, NDI 6.2 fails the duplicate create outright, and the old
// failure path's NDIlib_destroy() tore the process-wide NDI library out from
// under the healthy sender, leaking its Bonjour advertisement and locking the
// name machine-wide until Resolve exited. So:
//   - NDIlib_initialize() runs once per process and NDIlib_destroy() is never
//     called — destroying under live senders is what leaked the names, and the
//     library is reclaimed at process exit anyway;
//   - instances sharing a source name share ONE sender (refcounted), which is
//     also exactly what stereo needs: one packed frame per eye pair, sent once;
//   - the eye pairer lives here, keyed process-globally, because L and R
//     frames for the same timeline frame arrive on different instances.
// ---------------------------------------------------------------------------

struct SenderHub {
    std::string name;
    std::mutex mutex;                 // serializes pairer state and sends
    // Lock-free mirror of `sender != nullptr` for the per-render hot path:
    // the pump workers hold `mutex` for tens of ms (pairer copy + sync send),
    // and a render action must never queue behind that just to learn the
    // sender exists (v1.6.1 — this wait was the residual 8K playback drag).
    std::atomic<bool> senderReady{false};
    int refCount = 0;
    NDIlib_send_instance_t sender = nullptr;
    bool createAttempted = false;
    std::chrono::steady_clock::time_point lastCreateAttempt;
    bool asyncInFlight = false;       // an async send may still be reading its buffer
    ndi_stereo::EyePairer pairer;
    // Packed-pair output, double-buffered: an async send keeps reading the
    // submitted buffer until the next send, and alternating buffers means the
    // next pair never packs into the one still in flight.
    std::vector<uint8_t> packedBuffer[2];
    int packedIndex = 0;
    // Last packed frame actually sent (pair or duplicate) — RepeatLast
    // keepalives re-send it while an eye stalls (issue #12). The pairer
    // pre-gates repeats on the EYE meta it saw; this records the PACKED dims
    // actually sent, the layout-level double-check (the pairer never knows
    // the SbS/TB param).
    bool hasPackedFrame = false;
    ndi_stereo::FrameMeta lastPackedMeta;
    std::string status;               // last stream-status string, for change detection
    unsigned long long lastLoggedDrops = 0;
};

static std::mutex gHubRegistryMutex;
static std::map<std::string, SenderHub*> gHubRegistry;

#ifdef _WIN32
// The NDI import is delay-loaded (CMakeLists.txt /DELAYLOAD): resolution
// happens at the first NDI call, and an unresolvable DLL there raises the
// delay-load helper's SEH exception inside Resolve. So before any NDI call,
// load the runtime shipped inside the bundle from the plugin's own directory
// (module-relative; the loader never searches there on its own), falling back
// to a system-wide NDI runtime. On total failure NDI stays off and the
// plugin keeps working as a pass-through. See NDIRuntimeLoader.h.
static bool ensureNDIRuntimeLoaded()
{
    static const bool loaded = []() {
        const ndi_loader::PreloadResult r =
            ndi_loader::preloadNDIRuntime(L"Processing.NDI.Lib.Advanced.x64.dll");
        if (r.loaded && r.fromBundle) {
            NDI_LOG("NDI runtime loaded from the bundle: %ls", r.bundlePath.c_str());
        } else if (r.loaded) {
            NDI_LOG("NDI runtime not beside the plugin (%ls, Win32 error %lu); "
                    "using the system-installed runtime",
                    r.bundlePath.c_str(), r.bundleError);
        } else {
            NDI_LOG("NDI runtime NOT FOUND: bundle attempt %ls failed "
                    "(Win32 error %lu), system search failed (Win32 error %lu) "
                    "- streaming disabled",
                    r.bundlePath.c_str(), r.bundleError, r.systemError);
        }
        return r.loaded;
    }();
    return loaded;
}
#endif // _WIN32

// NDIlib_initialize is refcounted inside the SDK and safe to call once and
// keep; see the hub comment for why NDIlib_destroy must never be called.
static bool ensureNDILibInitialized()
{
    static std::mutex initMutex;
    static bool initialized = false;
    std::lock_guard<std::mutex> lock(initMutex);
    if (!initialized) {
#ifdef _WIN32
        if (!ensureNDIRuntimeLoaded()) {
            return false;
        }
#endif
        initialized = NDIlib_initialize();
        if (initialized) {
            NDI_LOG("NDI library initialized (process-wide, kept for process lifetime)");
        } else {
            NDI_LOG("Failed to initialize NDI library");
        }
    }
    return initialized;
}

// Create the sender if it doesn't exist yet, throttled: a name collision (the
// classic leaked-advertisement lockout, or another app holding the name) must
// not retry-and-log on every render call. Caller holds hub->mutex.
static bool hubEnsureSenderLocked(SenderHub* hub)
{
    if (hub->sender) {
        return true;
    }
    auto now = std::chrono::steady_clock::now();
    if (hub->createAttempted &&
        std::chrono::duration_cast<std::chrono::seconds>(now - hub->lastCreateAttempt).count() < 3) {
        return false;
    }
    hub->createAttempted = true;
    hub->lastCreateAttempt = now;

    NDIlib_send_create_t desc;
    desc.p_ndi_name = hub->name.c_str();
    desc.p_groups = nullptr;
    desc.clock_video = true;
    desc.clock_audio = false;
    hub->sender = NDIlib_send_create(&desc);
    if (!hub->sender) {
        NDI_LOG_TEXT(("Failed to create NDI sender '" + hub->name +
                      "' — the name may already be advertised on this machine "
                      "(dns-sd -B _ndi._tcp local.) or the NDI runtime is missing; retrying every 3s").c_str());
        return false;
    }
    hub->senderReady.store(true, std::memory_order_release);
    NDI_LOG_TEXT(("NDI sender created: '" + hub->name + "'").c_str());
    return true;
}

// Caller holds hub->mutex. Completes an in-flight async send (a NULL frame
// finishes the previous submission) so a buffer can be reallocated safely.
static void hubFlushAsyncLocked(SenderHub* hub)
{
    if (hub->asyncInFlight && hub->sender) {
        NDIlib_send_send_video_async_v2(hub->sender, nullptr);
    }
    hub->asyncInFlight = false;
}

static SenderHub* hubAcquire(const std::string& name)
{
    std::lock_guard<std::mutex> lock(gHubRegistryMutex);
    SenderHub*& slot = gHubRegistry[name];
    if (!slot) {
        slot = new SenderHub();
        slot->name = name;
    }
    ++slot->refCount;
    return slot;
}

static void hubRelease(SenderHub* hub)
{
    if (!hub) {
        return;
    }
    std::lock_guard<std::mutex> registryLock(gHubRegistryMutex);
    bool destroy = false;
    {
        std::lock_guard<std::mutex> lock(hub->mutex);
        // The departing instance's buffer may be the one in flight; complete
        // it before the buffer's owner goes away.
        hubFlushAsyncLocked(hub);
        destroy = (--hub->refCount == 0);
        if (destroy && hub->sender) {
            hub->senderReady.store(false, std::memory_order_release);
            NDIlib_send_destroy(hub->sender); // blocks until any in-flight send completes
            hub->sender = nullptr;
            NDI_LOG_TEXT(("NDI sender destroyed: '" + hub->name + "'").c_str());
        }
    }
    if (destroy) {
        gHubRegistry.erase(hub->name);
        delete hub;
    }
}

// An NDI async send keeps reading the previously submitted buffer until the
// next send call. Call this before any per-instance send buffer reallocates
// (e.g. the Resolution divisor changed mid-stream) so NDI never reads freed
// memory. Keyed on the hub's in-flight flag, not the live Async Sending
// param — toggling the param off must not skip the flush for a frame already
// submitted.
static void flushAsyncSend(NDIInstanceData* data)
{
    if (data->hub) {
        std::lock_guard<std::mutex> lock(data->hub->mutex);
        hubFlushAsyncLocked(data->hub);
    }
}

// Milliseconds since t0 — for the mtimer diagnostic stage timers (issue #5).
static inline double msSince(const std::chrono::steady_clock::time_point& t0)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// Caller holds hub->mutex. Builds and sends one NDI frame of the given wire
// format. `allowAsync` is false for P216 (HDR sends have always been
// synchronous here) and for any buffer that can't outlive the call.
// hdrMetadataXML arrives by reference from the submission (never read off the
// instance here — the pump worker calls this while render threads own the
// instance's HDR strings).
static void hubSendFrameLocked(SenderHub* hub, NDIInstanceData* data,
                               const ndi_stereo::FrameMeta& meta,
                               const uint8_t* bytes, bool allowAsync,
                               const std::string& hdrMetadataXML)
{
    NDIlib_video_frame_v2_t frame;
    frame.xres = meta.width;
    frame.yres = meta.height;
    frame.frame_rate_N = static_cast<int>(data->frameRate * 1000);
    frame.frame_rate_D = 1000;
    frame.picture_aspect_ratio = static_cast<float>(meta.width) / static_cast<float>(meta.height);
    frame.frame_format_type = NDIlib_frame_format_type_progressive;
    frame.timecode = NDIlib_send_timecode_synthesize;
    frame.p_data = const_cast<uint8_t*>(bytes);
    frame.p_metadata = nullptr;

    switch (meta.format) {
        case ndi_stereo::WireFormat::P216:
            frame.FourCC = NDIlib_FourCC_video_type_P216;
            frame.line_stride_in_bytes = meta.width * static_cast<int>(sizeof(uint16_t));
            frame.p_metadata = hdrMetadataXML.empty() ? nullptr : hdrMetadataXML.c_str();
            break;
        case ndi_stereo::WireFormat::RGBA8:
            frame.FourCC = NDIlib_FourCC_type_RGBA;
            frame.line_stride_in_bytes = meta.width * 4;
            break;
        case ndi_stereo::WireFormat::UYVY8:
        default:
            frame.FourCC = NDIlib_FourCC_type_UYVY;
            frame.line_stride_in_bytes = meta.width * 2;
            break;
    }

    if (allowAsync && data->asyncSending) {
        NDIlib_send_send_video_async_v2(hub->sender, &frame);
        hub->asyncInFlight = true;
    } else {
        NDIlib_send_send_video_v2(hub->sender, &frame);
        hub->asyncInFlight = false;
    }
}

// Caller holds hub->mutex. Human-readable stream status for the UI param.
static std::string hubComposeStatusLocked(SenderHub* hub, NDIInstanceData* data)
{
    if (!hub->sender) {
        return "No NDI sender — name '" + hub->name + "' unavailable (in use?)";
    }
    std::string status;
    switch (hub->pairer.mode()) {
        case ndi_stereo::StreamMode::Stereo:
            status = data->stereoPacking == 1 ? "Stereo (Top-Bottom)" : "Stereo (Side-by-Side)";
            break;
        case ndi_stereo::StreamMode::LeftOnly:
            status = "Stereo degraded: right eye stalled — left eye in both halves (canvas held)";
            break;
        case ndi_stereo::StreamMode::RightOnly:
            status = "Stereo degraded: left eye stalled — right eye in both halves (canvas held)";
            break;
        case ndi_stereo::StreamMode::Mono:
        default:
            status = "Mono";
            break;
    }
    const bool metadataMode = (data->projectionMode == 2);
    switch (data->projStatus.load(std::memory_order_relaxed)) {
        case kProjActive:
            status += metadataMode ? ", Equirect (Camera Metadata)" : ", Equirect (STMap)";
            break;
        case kProjError:
            status += metadataMode ? ", Camera metadata invalid — passthrough (see log)"
                                   : ", STMap invalid — passthrough (see log)";
            break;
        case kProjMismatch:
            status += ", STMap L/R size mismatch — passthrough";
            break;
        default:
            break;
    }
    return status;
}

// Caller holds hub->mutex. Recompose the stream status; log hub-level
// changes once, and mark the SUBMITTING instance's param dirty whenever ITS
// last-pushed value differs — change detection is per instance, so both
// per-eye instances (which back the same node param) converge on the live
// status regardless of which one noticed the transition.
static void hubUpdateStatusLocked(SenderHub* hub, NDIInstanceData* data)
{
    std::string status = hubComposeStatusLocked(hub, data);
    if (status != hub->status) {
        hub->status = status;
        NDI_LOG_TEXT(("Stream status: " + status).c_str());
    }
    std::lock_guard<std::mutex> statusLock(data->statusMutex); // after hub->mutex, never reversed
    if (status != data->statusParamValue) {
        data->statusParamValue = status;
        data->statusParamDirty = true;
    }
}

// One frame handed to the hub: everything the pairer and sender need,
// captured at the PRODUCING call site. The async pump submits after the
// originating render call has returned, so nothing here may be read off the
// instance's render*/HDR fields at consume time — they belong to a later
// render by then.
struct HubSubmit {
    ndi_stereo::WireFormat format = ndi_stereo::WireFormat::UYVY8;
    int width = 0;
    int height = 0;
    const uint8_t* bytes = nullptr;
    size_t byteCount = 0;
    bool allowAsync = false;
    int eye = ndi_stereo::kEyeLeft;
    double time = 0.0;
    bool isThumbnail = false;
    std::string hdrMetadataXML; // P216 only
};

// Stage timings for one hub submission. Out-param rather than instance fields
// because render threads and the pump worker submit concurrently — each
// caller owns its own copy (the render path copies into the mtimer fields,
// the worker logs a wtimer line).
struct SubmitTimers {
    double flushMs = 0.0, packMs = 0.0, sendMs = 0.0;
};

// Caller holds hub->mutex. Packs two eye buffers into the hub's alternate
// packed buffer and sends the result on the doubled canvas, remembering the
// packed meta for RepeatLast keepalives. `left` and `right` may be the same
// buffer — that is the degraded duplication path (issue #12).
static void hubPackAndSendLocked(SenderHub* hub, NDIInstanceData* data,
                                 const ndi_stereo::FrameMeta& meta,
                                 ndi_stereo::StereoLayout layout,
                                 const uint8_t* left, const uint8_t* right,
                                 bool allowAsync, const std::string& hdrMetadataXML,
                                 SubmitTimers* timers)
{
    // Pack into the buffer NOT submitted last; if it needs resizing it
    // could still be read by a send before last, so flush first.
    std::vector<uint8_t>& packed = hub->packedBuffer[hub->packedIndex ^= 1];
    const size_t packedBytes = ndi_stereo::wireFrameBytes(meta) * 2;
    if (packed.size() != packedBytes) {
        const auto flushT0 = std::chrono::steady_clock::now();
        hubFlushAsyncLocked(hub);
        if (timers) timers->flushMs = msSince(flushT0);
        packed.resize(packedBytes);
    }
    const auto packT0 = std::chrono::steady_clock::now();
    ndi_stereo::packStereoFrame(meta, layout, left, right, packed.data());
    if (timers) timers->packMs = msSince(packT0);

    ndi_stereo::FrameMeta packedMeta = meta;
    ndi_stereo::packedDims(meta, layout, &packedMeta.width, &packedMeta.height);
    hub->lastPackedMeta = packedMeta;
    hub->hasPackedFrame = true;
    const auto sendT0 = std::chrono::steady_clock::now();
    hubSendFrameLocked(hub, data, packedMeta, packed.data(), allowAsync, hdrMetadataXML);
    if (timers) timers->sendMs = msSince(sendT0);
}

// The single entry point every converted frame goes through. Consults the
// process-global pairer: mono frames stream unchanged (zero extra copies),
// stereo frames wait for their partner and go out as ONE packed frame.
static void hubSubmitFrame(NDIInstanceData* data, const HubSubmit& s, SubmitTimers* timers)
{
    SenderHub* hub = data->hub;
    if (!hub) {
        return;
    }

    std::lock_guard<std::mutex> lock(hub->mutex);
    if (!hubEnsureSenderLocked(hub)) {
        // Surface the failure — this is the user-visible symptom of the
        // sender-name lockout. (Defensive: the send paths normally bail in
        // ensureNDIReady before reaching here, which also updates status.)
        hubUpdateStatusLocked(hub, data);
        return;
    }

    ndi_stereo::FrameMeta meta;
    meta.width = s.width;
    meta.height = s.height;
    meta.format = s.format;

    const uint64_t nowMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());

    ndi_stereo::SubmitResult result = hub->pairer.submit(
        s.eye, s.time, meta, s.bytes, s.byteCount, nowMs, s.isThumbnail);

    const ndi_stereo::StereoLayout layout = (data->stereoPacking == 1)
                                                ? ndi_stereo::StereoLayout::TopBottom
                                                : ndi_stereo::StereoLayout::SideBySide;

    switch (result.action) {
        case ndi_stereo::SubmitAction::SendMono: {
            const auto sendT0 = std::chrono::steady_clock::now();
            hubSendFrameLocked(hub, data, meta, s.bytes, s.allowAsync, s.hdrMetadataXML);
            if (timers) timers->sendMs = msSince(sendT0);
            break;
        }

        case ndi_stereo::SubmitAction::SendPair: {
            const uint8_t* left = (s.eye == ndi_stereo::kEyeLeft)
                                      ? s.bytes : result.matePayload.data();
            const uint8_t* right = (s.eye == ndi_stereo::kEyeLeft)
                                       ? result.matePayload.data() : s.bytes;
            hubPackAndSendLocked(hub, data, meta, layout, left, right,
                                 s.allowAsync, s.hdrMetadataXML, timers);
            // Return the consumed mate buffer to the pairer's pool — its warm
            // pages make the next hold a plain memcpy instead of a page-fault
            // storm inside this mutex.
            hub->pairer.recycle(std::move(result.matePayload));
            break;
        }

        case ndi_stereo::SubmitAction::SendDuplicate: {
            // Degraded stereo (issue #12): the partner eye starved, but the
            // canvas keeps its packed dimensions — the flowing eye goes into
            // both halves. Viewers see 2D; the geometry never pops.
            hubPackAndSendLocked(hub, data, meta, layout, s.bytes, s.bytes,
                                 s.allowAsync, s.hdrMetadataXML, timers);
            break;
        }

        case ndi_stereo::SubmitAction::RepeatLast: {
            // Stall keepalive (issue #12): the submitted frame is held, and
            // the last packed frame goes out again so receivers keep getting
            // frames while the pairer waits out the degrade hysteresis. Only
            // when that frame still matches the canvas this submit implies —
            // a layout/resolution change mid-stall just holds instead.
            ndi_stereo::FrameMeta packedMeta = meta;
            ndi_stereo::packedDims(meta, layout, &packedMeta.width, &packedMeta.height);
            std::vector<uint8_t>& packed = hub->packedBuffer[hub->packedIndex];
            if (hub->hasPackedFrame && hub->lastPackedMeta == packedMeta &&
                packed.size() == ndi_stereo::wireFrameBytes(packedMeta)) {
                // Re-sending the buffer already designated live is safe under
                // the async contract: it stays the one NDI may read from.
                const auto sendT0 = std::chrono::steady_clock::now();
                hubSendFrameLocked(hub, data, packedMeta, packed.data(),
                                   s.allowAsync, s.hdrMetadataXML);
                if (timers) timers->sendMs = msSince(sendT0);
            } else {
                const auto flushT0 = std::chrono::steady_clock::now();
                hubFlushAsyncLocked(hub);
                if (timers) timers->flushMs = msSince(flushT0);
            }
            break;
        }

        case ndi_stereo::SubmitAction::Hold:
        case ndi_stereo::SubmitAction::Drop:
        default: {
            // No send this call: complete any in-flight async send now. Its
            // buffer belongs to an instance that will overwrite it on its
            // next conversion, and with Hold the next real send could be
            // arbitrarily far away — the async-buffer rule (LEARNINGS
            // 2026-08-28) demands a send or NULL flush closes every window.
            const auto flushT0 = std::chrono::steady_clock::now();
            hubFlushAsyncLocked(hub);
            if (timers) timers->flushMs = msSince(flushT0);
            break;
        }
    }

    if (hub->pairer.droppedFrames() != hub->lastLoggedDrops) {
        NDI_LOG("Stereo pairer dropped %llu unmated frame(s) total",
                hub->pairer.droppedFrames());
        hub->lastLoggedDrops = hub->pairer.droppedFrames();
    }

    hubUpdateStatusLocked(hub, data);
}

#ifdef NDI_GPU_NATIVE
// ---------------------------------------------------------------------------
// Async NDI pump (issue #5, v1.6.0; un-gated from Apple-only by ticket #22 —
// it consumes Metal and CUDA completions identically through the GPU-native
// seam). Diagnosis showed the 8K stereo playback collapse (30fps -> 5fps) was
// ~90ms of blocking inside each render action — the GPU wait plus the
// CPU-side NDI work. Now the render action only ENQUEUES the fused
// downscale+convert kernel (microseconds); the GPU module's completion
// callback queues the finished staging slot here, and this per-instance
// worker does everything else — pairing, packing, the NDI send — off the
// render thread. Every worker send is synchronous, so no NDI in-flight buffer
// ever aliases a staging slot; the async-send flush rules stay confined to
// the legacy blocking paths. Backpressure anywhere (slot ring, this queue)
// DROPS the frame — an NDI preview must never block the host.
// ---------------------------------------------------------------------------
struct AsyncPumpItem {
    void* slot = nullptr;
    NativeGPUContextRef nativeContext = nullptr;
    HubSubmit submit;  // bytes/byteCount are filled by the completion callback
    double gpuMs = 0.0;
    bool ok = false;
};

struct AsyncPump {
    NDIInstanceData* data = nullptr; // owner; outlives the pump (shutdownNDI drains first)
    std::mutex m;
    std::condition_variable cv;
    std::queue<AsyncPumpItem> queue; // bounded by kAsyncPumpQueueCap
    std::atomic<int> pendingSubmits{0}; // GPU callbacks not yet finished with this pump
    bool stop = false;
    std::thread worker;
    std::atomic<uint64_t> drops{0};  // incremented from render threads AND the callback
    std::chrono::steady_clock::time_point lastDropLog{}; // render threads only
};

static const size_t kAsyncPumpQueueCap = 4;

// The per-render values the pairer needs, captured at ENQUEUE time — the
// instance's render* fields belong to a later call by completion time.
struct AsyncSubmitCtx {
    AsyncPump* pump = nullptr;
    AsyncPumpItem item;
};

static void pumpWorkerLoop(AsyncPump* pump)
{
    NDIInstanceData* data = pump->data;
    for (;;) {
        AsyncPumpItem item;
        size_t depth = 0;
        {
            std::unique_lock<std::mutex> lock(pump->m);
            pump->cv.wait(lock, [pump] { return pump->stop || !pump->queue.empty(); });
            if (pump->stop) {
                return; // leftovers are released by pumpShutdown
            }
            item = pump->queue.front();
            pump->queue.pop();
            depth = pump->queue.size();
        }
        if (item.ok) {
            // HDR metadata was captured into item.submit at ENQUEUE time on
            // the render thread — this worker never reads the instance's
            // colorSpace/transfer strings (they race with instanceChanged).
            // Status changes are only FLAGGED inside the hub (statusParamDirty)
            // — paramSetValue is a host call and stays on render threads.
            const auto t0 = std::chrono::steady_clock::now();
            SubmitTimers timers;
            hubSubmitFrame(data, item.submit, &timers);
            if (data->timersEnabled) {
                NDI_LOG("wtimer eye=%d gpu=%.2f pack=%.1f send=%.1f total=%.1f qdepth=%zu",
                        item.submit.eye, item.gpuMs, timers.packMs, timers.sendMs,
                        msSince(t0), depth);
            }
        }
        nativeGpuDownscaleRelease(item.nativeContext, item.slot);
    }
}

static AsyncPump* pumpEnsure(NDIInstanceData* data)
{
    if (!data->pump) {
        data->pump = new AsyncPump();
        data->pump->data = data;
        data->pump->worker = std::thread(pumpWorkerLoop, data->pump);
    }
    return data->pump;
}

// GPU completion callback (Metal's completion thread / the CUDA module's
// dispatcher). Anything slow here stalls the host's own completion handlers,
// so it only queues the slot and wakes the worker.
static void pumpOnConvertDone(void* user, void* slot, const void* outPtr,
                              size_t outBytes, double gpuMs, bool ok)
{
    AsyncSubmitCtx* ctx = static_cast<AsyncSubmitCtx*>(user);
    AsyncPump* pump = ctx->pump;
    ctx->item.slot = slot;
    ctx->item.submit.bytes = static_cast<const uint8_t*>(outPtr);
    ctx->item.submit.byteCount = outBytes;
    ctx->item.gpuMs = gpuMs;
    ctx->item.ok = ok;

    bool queued = false;
    {
        std::lock_guard<std::mutex> lock(pump->m);
        // Queue-full is belt-and-braces: the cap equals the slot-ring size and
        // every queued item pins a distinct busy slot, so today it can't hit.
        if (!pump->stop && pump->queue.size() < kAsyncPumpQueueCap) {
            pump->queue.push(std::move(ctx->item));
            queued = true;
        } else if (!pump->stop) {
            ++pump->drops;
        }
    }
    if (queued) {
        pump->cv.notify_one();
    } else {
        nativeGpuDownscaleRelease(ctx->item.nativeContext, slot);
    }
    delete ctx;
    --pump->pendingSubmits; // last touch: pumpShutdown waits on this before freeing
}

static void pumpShutdown(NDIInstanceData* data)
{
    AsyncPump* pump = data->pump;
    if (!pump) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(pump->m);
        pump->stop = true;
    }
    pump->cv.notify_all();
    if (pump->worker.joinable()) {
        pump->worker.join();
    }
    // Callbacks for still-executing command buffers release their own slots
    // once stop is set; wait them out so none touches a freed pump.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (pump->pendingSubmits.load() > 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    {
        // Worker exits on stop with items possibly still queued — release them.
        std::lock_guard<std::mutex> lock(pump->m);
        while (!pump->queue.empty()) {
            nativeGpuDownscaleRelease(pump->queue.front().nativeContext, pump->queue.front().slot);
            pump->queue.pop();
        }
    }
    if (pump->pendingSubmits.load() > 0) {
        NDI_LOG("Async pump: %d GPU callback(s) never arrived — leaking pump to stay safe",
                pump->pendingSubmits.load());
        data->pump = nullptr;
        return;
    }
    delete pump;
    data->pump = nullptr;
}
#endif // NDI_GPU_NATIVE

// GPU Acceleration Functions
static bool initializeGPUContext(NDIInstanceData* data)
{
    if (!data->gpuAcceleration) {
        return true; // GPU acceleration disabled
    }

    NDI_LOG("Initializing GPU acceleration...");
    
    data->gpuContext = std::make_unique<GPUContext>();
    data->gpuContext->initialized = false;

#ifdef NDI_GPU_NATIVE
    // Metal on macOS, CUDA on Windows — same seam. Unavailable (e.g. a
    // non-NVIDIA Windows machine) is not an error: the caller falls back to
    // the CPU conversion path (spec decision 4's documented ceiling).
    if (!nativeGpuAvailable()) {
        NDI_LOG(NDI_GPU_BACKEND_NAME " is not available on this system - using the CPU path");
        return false;
    }

    data->gpuContext->nativeContext = nativeGpuInit();
    if (!data->gpuContext->nativeContext) {
        NDI_LOG("Failed to initialize " NDI_GPU_BACKEND_NAME " GPU acceleration");
        return false;
    }

    NDI_LOG(NDI_GPU_BACKEND_NAME " GPU acceleration initialized successfully");

#else
    // No GPU-native module in this build — the CPU conversion path renders.
    NDI_LOG("GPU-native path not available on this platform - using the CPU path");
    return false;
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

#ifdef NDI_GPU_NATIVE
    if (data->gpuContext->nativeContext) {
        nativeGpuShutdown(data->gpuContext->nativeContext);
        data->gpuContext->nativeContext = nullptr;
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
        flushAsyncSend(data);
        data->uyvyFrameBuffer.resize(uyvySize);
    }

#ifdef __APPLE__
    // Use Metal GPU acceleration for RGBA to UYVY conversion
    if (data->gpuContext->nativeContext) {
        NDI_LOG("🚀 Attempting Metal GPU acceleration...\n");

        bool success = metal_gpu_convert_rgba_to_uyvy(
            data->gpuContext->nativeContext,
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

#else
    // CPU-buffer renders convert on the CPU everywhere but macOS; the
    // Windows GPU-native path (CUDA, ticket #22) runs through the render
    // action's device-buffer fast path instead, never through here.
    convertRGBAToUYVY_CPU(data, rgbaData, width, height);
#endif
}

static void convertRGBAToUYVY_CPU(NDIInstanceData* data, void* rgbaData, int width, int height)
{
    auto startTime = std::chrono::high_resolution_clock::now();
    
    NDI_LOG("Starting CPU RGBA->UYVY conversion (%dx%d)\n", width, height);

    const size_t uyvySize = width * height * 2; // UYVY is 2 bytes per pixel
    if (data->uyvyFrameBuffer.size() != uyvySize) {
        flushAsyncSend(data);
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

    if (!ensureNDILibInitialized()) {
        return false;
    }

    // Attach to the process-shared sender for this source name. Sender
    // creation itself is attempted lazily (and throttled) inside the hub so a
    // locked name never spams create attempts per render call.
    data->hub = hubAcquire(data->sourceName);
    {
        std::lock_guard<std::mutex> lock(data->hub->mutex);
        if (hubEnsureSenderLocked(data->hub) && data->gpuAcceleration) {
            // Hardware acceleration metadata hint, once per sender creation.
            const char* hwAccelMetadata = "<ndi_video_codec type=\"hardware\"/>";
            NDIlib_metadata_frame_t metadataFrame;
            metadataFrame.length = strlen(hwAccelMetadata);
            metadataFrame.timecode = NDIlib_send_timecode_synthesize;
            metadataFrame.p_data = const_cast<char*>(hwAccelMetadata);
            NDIlib_send_send_metadata(data->hub->sender, &metadataFrame);
        }
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
    NDI_LOG_TEXT(("NDI attached with source name '" + data->sourceName + "'").c_str());
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

#ifdef NDI_GPU_NATIVE
    // Drain the async pump FIRST: its worker submits into the hub and its
    // GPU callbacks write staging slots — both must be quiet before the
    // GPU context and the hub go away.
    pumpShutdown(data);
#endif

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

    // Detach from the shared sender; it is destroyed only when the last
    // instance lets go. NDIlib_destroy() is deliberately NEVER called — see
    // the SenderHub comment (it leaked sender-name advertisements machine-wide).
    hubRelease(data->hub);
    data->hub = nullptr;
    data->ndiInitialized = false;
}

// Build the ndi_color_info XML from the instance's color settings. RENDER
// THREADS ONLY — it reads the colorSpace/transferFunction strings that
// instanceChanged rewrites; the async pump captures the RESULT into its item
// at enqueue time instead of calling this from the worker.
static std::string composeHDRMetadataXML(NDIInstanceData* data)
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
    return "<ndi_color_info primaries=\"" + primaries +
           "\" transfer=\"" + transfer +
           "\" matrix=\"" + matrix + "\" />";
}

static void createHDRMetadata(NDIInstanceData* data)
{
    data->hdrMetadataXML = composeHDRMetadataXML(data);
    NDI_LOG("HDR Metadata: %s", data->hdrMetadataXML.c_str());
}

// Submit the already-packed UYVY frame in data->uyvyFrameBuffer to the hub
// (which streams it as mono, or pairs and packs it in stereo). Used by both
// conversion paths: CPU/upload-convert (sendSDRFrame) and the GPU-native
// fused downscale+convert (renderGPUFrame).
// Shared tail of the legacy blocking paths — these run on the render thread,
// so data->render* is current and the stage timings land in the mtimer fields.
static void hubSubmitFromRenderThread(NDIInstanceData* data, const HubSubmit& s)
{
    SubmitTimers timers;
    hubSubmitFrame(data, s, &timers);
    data->timerFlushMs = timers.flushMs;
    data->timerPackMs = timers.packMs;
    data->timerSendMs = timers.sendMs;
}

static void sendUYVYToNDI(NDIInstanceData* data, int width, int height)
{
    HubSubmit s;
    s.format = ndi_stereo::WireFormat::UYVY8;
    s.width = width;
    s.height = height;
    s.bytes = data->uyvyFrameBuffer.data();
    s.byteCount = static_cast<size_t>(width) * height * 2;
    s.allowAsync = true;
    s.eye = data->renderEye;
    s.time = data->renderTime;
    s.isThumbnail = data->renderIsThumbnail;
    hubSubmitFromRenderThread(data, s);
}

// Submit the already-packed P216 frame in data->hdrFrameBuffer with HDR
// metadata. HDR sends stay synchronous (as they always were here).
static void sendP216ToNDI(NDIInstanceData* data, int width, int height)
{
    createHDRMetadata(data);
    HubSubmit s;
    s.format = ndi_stereo::WireFormat::P216;
    s.width = width;
    s.height = height;
    s.bytes = reinterpret_cast<const uint8_t*>(data->hdrFrameBuffer.data());
    s.byteCount = static_cast<size_t>(width) * height * 2 * sizeof(uint16_t);
    s.allowAsync = false;
    s.eye = data->renderEye;
    s.time = data->renderTime;
    s.isThumbnail = data->renderIsThumbnail;
    s.hdrMetadataXML = data->hdrMetadataXML; // same thread — safe to copy here
    hubSubmitFromRenderThread(data, s);
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
    if (data->gpuAcceleration && data->gpuContext && data->gpuContext->initialized && data->gpuContext->nativeContext) {
        // For HDR, we need to convert to 16-bit limited range
        // The scale factor should be for 16-bit limited range (not full range)
        float scale = 65472.0f; // 16-bit limited range: (235-16) * 256 + (240-16) * 256 for chroma
        
        gpuSuccess = metal_gpu_convert_rgba_to_hdr(
            data->gpuContext->nativeContext,
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

    // Send the HDR frame
    sendP216ToNDI(data, width, height);
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

    if (data->optimalFormat) {
        // Use UYVY format for optimal NDI performance
        if (data->gpuAcceleration) {
            convertRGBAToUYVY_GPU(data, imageData, width, height);
        } else {
            convertRGBAToUYVY_CPU(data, imageData, width, height);
        }
        sendUYVYToNDI(data, width, height);
        return;
    }

    // Use RGBA format (legacy compatibility)
    const size_t frameSize = width * height * 4 * sizeof(uint8_t);
    if (data->frameBuffer.size() != frameSize) {
        flushAsyncSend(data);
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

    HubSubmit s;
    s.format = ndi_stereo::WireFormat::RGBA8;
    s.width = width;
    s.height = height;
    s.bytes = dstData;
    s.byteCount = frameSize;
    s.allowAsync = true;
    s.eye = data->renderEye;
    s.time = data->renderTime;
    s.isThumbnail = data->renderIsThumbnail;
    hubSubmitFromRenderThread(data, s);
}

static bool ensureNDIReady(NDIInstanceData* data)
{
    if (!data->enabled) {
        return false;
    }
    if (!data->ndiInitialized) {
        NDI_LOG("NDI not initialized, attempting to initialize...");
        if (!initializeNDI(data)) {
            NDI_LOG("Failed to initialize NDI, skipping frame");
            return false;
        }
    }
    // Hot path: a healthy sender answers without touching hub->mutex — the
    // pump workers hold that lock for tens of ms per frame, and a render
    // action queueing behind them was the residual 8K playback drag (v1.6.1).
    // hubSubmitFrame re-verifies under the lock, so a sender dying between
    // this check and the submit is still caught.
    if (data->hub->senderReady.load(std::memory_order_acquire)) {
        return true;
    }
    // Slow path — a missing sender (name locked elsewhere) skips the
    // conversion work too; creation retries are throttled inside the hub.
    std::lock_guard<std::mutex> lock(data->hub->mutex);
    const bool ready = hubEnsureSenderLocked(data->hub);
    if (!ready) {
        // No frame will be submitted this render, so surface the lockout in
        // Stream Status from here — hubSubmitFrame won't get the chance.
        hubUpdateStatusLocked(data->hub, data);
    }
    return ready;
}

static void sendNDIFrame(NDIInstanceData* data, void* imageData, int width, int height)
{
    if (!ensureNDIReady(data)) {
        return;
    }

    if (data->hdrEnabled) {
        sendHDRFrame(data, imageData, width, height);
    } else {
        sendSDRFrame(data, imageData, width, height);
    }
}

// Push a changed stream-status string to the informational UI param, at the
// end of the render action and only when it actually changed (rare). Kept as
// one isolated helper: hosts differ on tolerating paramSetValue during
// render, so this is trivially removable if a host objects — the same status
// always goes to the log too.
static void flushStatusParam(NDIInstanceData* data)
{
    std::string statusCopy;
    {
        std::lock_guard<std::mutex> lock(data->statusMutex);
        if (!data->statusParamDirty) {
            return;
        }
        data->statusParamDirty = false;
        statusCopy = data->statusParamValue; // the worker mutates the original
    }
    if (data->stereoStatusParam) {
        gParamHost->paramSetValue(data->stereoStatusParam, statusCopy.c_str());
    }
}

// Frame arrived in CPU memory: apply the Resolution downscale (box filter) —
// or, in Equirect mode, the STMap warp — before the conversion+send path.
// Also repacks a padded row stride — the converters assume tight rows.
static void sendCPUFrameToNDI(NDIInstanceData* data, void* imageData, int width, int height, int rowBytes)
{
    int rowFloats = rowBytes / static_cast<int>(sizeof(float));
    if (rowFloats < width * 4) {
        rowFloats = width * 4; // defensive: hosts hand tight positive strides here
    }

    const int divisor = data->resolutionDivisor;

    // Equirect (STMap) projection (issue #7): the warp replaces the plain
    // downscale — the map defines the destination image, so output dimensions
    // are the MAP's, with the Resolution divisor applied on top. This is the
    // fallback-quality path (full CPU warp); the Metal render path warps on
    // the GPU.
    if (const StmapEntry* stmap = data->renderStmap.get()) {
        int outWidth = 0, outHeight = 0;
        ndi_stream::outputDims(stmap->map.width, stmap->map.height, divisor, &outWidth, &outHeight);
        const size_t outFloats = static_cast<size_t>(outWidth) * outHeight * 4;
        if (data->downscaleBuffer.size() < outFloats) {
            data->downscaleBuffer.resize(outFloats);
        }
        ndi_stmap::warpRGBABox(static_cast<const float*>(imageData), width, height, rowFloats,
                               stmap->map.uv.data(), stmap->map.width, stmap->map.height,
                               divisor, data->downscaleBuffer.data(), outWidth, outHeight);
        NDI_LOG("CPU STMap warp: %dx%d -> %dx%d (divisor %d)",
                width, height, outWidth, outHeight, divisor);
        sendNDIFrame(data, data->downscaleBuffer.data(), outWidth, outHeight);
        return;
    }

    if (divisor <= 1 && rowFloats == width * 4) {
        sendNDIFrame(data, imageData, width, height);
        return;
    }

    int outWidth = 0, outHeight = 0;
    ndi_stream::outputDims(width, height, divisor, &outWidth, &outHeight);

    const size_t outFloats = static_cast<size_t>(outWidth) * outHeight * 4;
    if (data->downscaleBuffer.size() < outFloats) {
        data->downscaleBuffer.resize(outFloats);
    }
    ndi_stream::downscaleRGBABox(static_cast<const float*>(imageData), width, height, rowFloats,
                                 divisor, data->downscaleBuffer.data(), outWidth, outHeight);
    NDI_LOG("CPU downscale: %dx%d -> %dx%d (divisor %d)", width, height, outWidth, outHeight, divisor);
    sendNDIFrame(data, data->downscaleBuffer.data(), outWidth, outHeight);
}

#ifdef NDI_GPU_NATIVE
// GPU render action (issue #5; CUDA on Windows via ticket #22): the host
// handed src/dst as device buffers (id<MTLBuffer> on macOS, CUDA device
// pointers on Windows). Passthrough-copy src→dst on the host's queue/stream
// for the effect output, then feed NDI through the fused GPU
// downscale+convert kernels — the downscale happens before any readback, so
// only the small converted frame crosses to the CPU. Any GPU-convert gap
// (legacy RGBA format, GPU Acceleration off, kernel failure) falls back to a
// full-frame readback plus the CPU path, so the stream survives every
// combination. In Equirect mode (issue #7) the fused kernel is the STMap warp
// variant and the output takes the MAP's dimensions; the readback fallback
// then warps on the CPU instead, so the stream stays geometrically correct on
// every path.
static OfxStatus renderGPUFrame(NDIInstanceData* data, void* srcBuffer, void* dstBuffer,
                                int width, int height, int srcRowBytes, int dstRowBytes,
                                void* gpuQueue)
{
    if (!srcBuffer || !dstBuffer) {
        NDI_LOG("GPU render: missing device buffer, skipping frame");
        return kOfxStatOK; // matches the CPU path's leniency for absent images
    }

    // GPU Acceleration may have been enabled after NDI init skipped the context.
    if (data->gpuAcceleration && !data->gpuContext) {
        initializeGPUContext(data);
    }
    NativeGPUContextRef nativeContext =
        (data->gpuContext && data->gpuContext->initialized) ? data->gpuContext->nativeContext : nullptr;

    // Host output first: the effect is a passthrough. No wait — the host
    // orders its downstream reads on the same queue (cf. the Resolve
    // GainPlugin sample).
    const size_t frameBytes = static_cast<size_t>(height) * static_cast<size_t>(dstRowBytes);
    const auto blitT0 = std::chrono::steady_clock::now();
    const bool blitOk = nativeGpuCopyBuffer(nativeContext, gpuQueue, srcBuffer, dstBuffer, frameBytes, false);
    data->timerBlitMs = msSince(blitT0);
    if (!blitOk) {
        NDI_LOG("GPU render: passthrough copy failed");
        return kOfxStatFailed;
    }

    if (!ensureNDIReady(data)) {
        return kOfxStatOK;
    }

    const int divisor = data->resolutionDivisor;
    const StmapEntry* stmap = data->renderStmap.get();
    int outWidth = 0, outHeight = 0;
    if (stmap) {
        // Equirect: the map defines the destination image.
        ndi_stream::outputDims(stmap->map.width, stmap->map.height, divisor, &outWidth, &outHeight);
    } else {
        ndi_stream::outputDims(width, height, divisor, &outWidth, &outHeight);
    }
    const int rowFloats = srcRowBytes / static_cast<int>(sizeof(float));

    bool handledByFastPath = false;
    if (data->gpuAcceleration && nativeContext &&
        (data->hdrEnabled || data->optimalFormat)) {
        // Non-blocking fast path (v1.6.0): ENQUEUE the fused kernel and
        // return. No GPU wait here — that wait (plus the CPU-side NDI work
        // that followed it) was ~90ms of render-thread blocking per eye and
        // the whole 8K playback collapse (#5). The pump worker pairs and
        // sends when the GPU finishes. The submit validates geometry itself:
        // BUSY = ring full (drop this frame, transient); INVALID = this
        // source can't fuse (fall through to the blocking readback so the
        // stream survives, every frame).
        std::lock_guard<std::mutex> lock(data->gpuContext->gpuMutex);
        AsyncPump* pump = pumpEnsure(data);
        const bool wantP216 = data->hdrEnabled;
        AsyncSubmitCtx* ctx = new AsyncSubmitCtx();
        ctx->pump = pump;
        ctx->item.nativeContext = nativeContext;
        ctx->item.submit.format = wantP216 ? ndi_stereo::WireFormat::P216
                                           : ndi_stereo::WireFormat::UYVY8;
        ctx->item.submit.width = outWidth;
        ctx->item.submit.height = outHeight;
        ctx->item.submit.allowAsync = false; // worker sends are synchronous by design
        ctx->item.submit.eye = data->renderEye;
        ctx->item.submit.time = data->renderTime;
        ctx->item.submit.isThumbnail = data->renderIsThumbnail;
        if (wantP216) {
            // Captured here, on the render thread — the worker must never
            // read the instance's color strings (they race instanceChanged).
            ctx->item.submit.hdrMetadataXML = composeHDRMetadataXML(data);
        }
        ++pump->pendingSubmits;
        const auto convT0 = std::chrono::steady_clock::now();
        NativeSubmitStatus st = kNativeSubmitInvalid;
        if (stmap) {
            // Warp path: a failed map upload never reaches the submit — it
            // stays INVALID and falls through to the readback + CPU warp, so
            // the stream keeps its corrected geometry (slowly) either way.
            void* mapBuffer = stmapNativeBufferForQueue(data->renderStmap, nativeContext, gpuQueue);
            if (mapBuffer) {
                st = nativeGpuWarpSubmit(nativeContext, gpuQueue, srcBuffer,
                                         width, height, rowFloats,
                                         mapBuffer, stmap->map.width, stmap->map.height,
                                         divisor, outWidth, outHeight, wantP216,
                                         pumpOnConvertDone, ctx);
            }
        } else {
            st = nativeGpuDownscaleSubmit(nativeContext, gpuQueue, srcBuffer,
                                          width, height, rowFloats, divisor,
                                          outWidth, outHeight, wantP216,
                                          pumpOnConvertDone, ctx);
        }
        data->timerConvMs = msSince(convT0);
        if (st == kNativeSubmitOK) {
            handledByFastPath = true;
            NDI_LOG("GPU-native async: %dx%d device frame -> %dx%d %d enqueued (divisor %d, fmt 0=UYVY 1=P216, warp %d)",
                    width, height, outWidth, outHeight, wantP216 ? 1 : 0, divisor, stmap ? 1 : 0);
        } else {
            --pump->pendingSubmits;
            delete ctx;
            if (st == kNativeSubmitBusy) {
                handledByFastPath = true; // deliberate drop — backpressure, not failure
                const uint64_t drops = ++pump->drops;
                const auto now = std::chrono::steady_clock::now();
                if (now - pump->lastDropLog > std::chrono::seconds(1)) {
                    pump->lastDropLog = now;
                    NDI_LOG("Async pump: frame dropped, %llu total (GPU or NDI worker behind)",
                            static_cast<unsigned long long>(drops));
                }
            }
            // kNativeSubmitInvalid falls through to the readback path below.
        }
    }

    if (!handledByFastPath) {
        const size_t srcBytes = static_cast<size_t>(height) * static_cast<size_t>(srcRowBytes);
        if (data->readbackBuffer.size() * sizeof(float) < srcBytes) {
            data->readbackBuffer.resize(srcBytes / sizeof(float));
        }
        const auto convT0 = std::chrono::steady_clock::now();
        const bool readOk = nativeGpuReadBuffer(nativeContext, gpuQueue, srcBuffer,
                                                data->readbackBuffer.data(), srcBytes);
        data->timerConvMs = msSince(convT0);
        if (readOk) {
            NDI_LOG("Device frame full readback -> CPU fallback path (%dx%d, gpu=%d)",
                    width, height, data->gpuAcceleration ? 1 : 0);
            sendCPUFrameToNDI(data, data->readbackBuffer.data(), width, height, srcRowBytes);
        } else {
            NDI_LOG("Device frame readback failed — NDI frame skipped");
        }
    }

    return kOfxStatOK;
}
#endif // NDI_GPU_NATIVE

// Plugin functions
static OfxStatus onLoad(void)
{
    return fetchHostSuites();
}

static OfxStatus onUnLoad(void)
{
#ifdef __APPLE__
    ndi_timelinewatch::shutdown();
#endif
    return kOfxStatOK;
}

static int brawMapSizeFromChoice(int choice)
{
    return (choice == 0) ? 1024 : (choice == 2) ? 4096 : 2048;
}

// ---------------------------------------------------------------------------
// Auto camera-clip mode (issue #11): a process-wide watcher follows the clip
// under the Resolve playhead (src/TimelineClipWatcher.h); registered
// instances in Timeline (Auto) source re-derive their lens maps whenever it
// changes. The registry mutex serializes the watcher's walk against
// createInstance/destroyInstance, so an instance can never die mid-apply.
// ---------------------------------------------------------------------------

static std::mutex gInstanceRegistryMutex;
static std::vector<NDIInstanceData*> gInstanceRegistry;

static void instanceRegistryAdd(NDIInstanceData* data)
{
    std::lock_guard<std::mutex> lock(gInstanceRegistryMutex);
    if (std::find(gInstanceRegistry.begin(), gInstanceRegistry.end(), data) ==
        gInstanceRegistry.end()) {
        gInstanceRegistry.push_back(data);
    }
}

static void instanceRegistryRemove(NDIInstanceData* data)
{
    std::lock_guard<std::mutex> lock(gInstanceRegistryMutex);
    gInstanceRegistry.erase(
        std::remove(gInstanceRegistry.begin(), gInstanceRegistry.end(), data),
        gInstanceRegistry.end());
}

#ifdef __APPLE__
// Runs on the watcher thread: re-source this instance's lens maps for the
// clip now under the playhead. Sticky on anything unusable — a gap between
// clips, a non-BRAW clip, a BRAW without calibration — the previous camera's
// maps keep flowing (a monitoring stream must never pop its geometry at a
// cut); each offending path logs its skip once. Map (re)generation is a
// cache hit for every camera seen before, so cuts between known cameras
// swap maps in microseconds.
static void applyAutoLensClip(NDIInstanceData* data, const std::string& path)
{
    if (!data->autoLensWanted.load(std::memory_order_relaxed)) {
        return;
    }
    const int mapSize = data->autoLensMapSize.load(std::memory_order_relaxed);
    const bool mask = data->autoLensMask.load(std::memory_order_relaxed);

    if (path.empty()) {
        std::lock_guard<std::mutex> lock(data->stmapMutex);
        if (data->autoLensSkippedPath != "\n(none)") {  // '\n' can't occur in a real path
            data->autoLensSkippedPath = "\n(none)";
            NDI_LOG("Auto camera clip: nothing under the playhead — keeping the current calibration");
        }
        return;
    }
    {
        std::lock_guard<std::mutex> lock(data->stmapMutex);
        if (data->autoLensAppliedPath == path &&
            data->stmapLeft && data->stmapLeft->valid) {
            return;  // already following this clip
        }
    }

    std::shared_ptr<StmapEntry> left, right;
    brawAcquireLensPair(path, mapSize, mask, &left, &right);
    if (left && left->valid && right && right->valid) {
        std::shared_ptr<StmapEntry> oldLeft, oldRight;
        {
            std::lock_guard<std::mutex> lock(data->stmapMutex);
            oldLeft = std::move(data->stmapLeft);
            oldRight = std::move(data->stmapRight);
            data->stmapLeft = left;
            data->stmapRight = right;
            data->autoLensAppliedPath = path;
            data->autoLensSkippedPath.clear();
        }
        data->projStatus.store(kProjActive, std::memory_order_relaxed);
        NDI_LOG_TEXT(("Auto camera clip: following '" + path + "'").c_str());
    } else {
        bool haveMaps = false, firstSkip = false;
        {
            std::lock_guard<std::mutex> lock(data->stmapMutex);
            haveMaps = data->stmapLeft && data->stmapLeft->valid &&
                       data->stmapRight && data->stmapRight->valid;
            firstSkip = (data->autoLensSkippedPath != path);
            data->autoLensSkippedPath = path;
        }
        if (!haveMaps) {
            data->projStatus.store(kProjError, std::memory_order_relaxed);
        }
        if (firstSkip) {
            const std::string why =
                (left && !left->error.empty()) ? left->error : "no calibration";
            NDI_LOG_TEXT(("Auto camera clip: '" + path + "' unusable (" + why + ") — " +
                          (haveMaps ? "keeping the current calibration" : "passthrough"))
                             .c_str());
        }
    }
}

// Watcher change callback (watcher thread): fan the new playhead clip out to
// every instance that wants auto mode.
static void timelineClipChanged(const std::string& path)
{
    std::lock_guard<std::mutex> lock(gInstanceRegistryMutex);
    for (NDIInstanceData* data : gInstanceRegistry) {
        applyAutoLensClip(data, path);
    }
}
#endif // __APPLE__

// Bring the instance's loaded STMaps in line with the current parameter
// values (issue #7). Called from createInstance and instanceChanged, never
// render — a multi-hundred-MB EXR load takes real time. Cache hits are a
// stat() and a map lookup, so calling on every parameter edit is cheap and
// doubles as the reload path when a map file is overwritten on disk.
//
// Warp engages only when the LEFT map is valid (the right map is optional and
// defaults to the left) and, when both are set, their dimensions agree —
// otherwise the two eyes would produce different frame sizes and the stereo
// pairer could never mate them. Every failure is soft: passthrough plus a
// Stream Status message, never a refusal to stream.
static void refreshSTMaps(NDIInstanceData* data)
{
    const bool wantStmap = (data->projectionMode == 1);
    const bool wantMetadata = (data->projectionMode == 2);
    const bool packedLayout = (data->stmapLayout == 1);
    const bool autoSource = (data->brawSourceChoice == 0);
    // Packed layout reads the single STMap field; projects saved before that
    // field existed carried the packed file in the left-eye slot, so an empty
    // packed field falls back there.
    const std::string& packedPath = !data->stmapPackedPathWanted.empty()
        ? data->stmapPackedPathWanted : data->stmapLeftPathWanted;
    const std::string& stmapPathWanted = packedLayout ? packedPath : data->stmapLeftPathWanted;
    std::shared_ptr<StmapEntry> left, right;
    std::string metadataClipPath;
    if (wantStmap && !stmapPathWanted.empty()) {
        if (packedLayout) {
            // One packed side-by-side file yields both eyes; the per-eye
            // slots are ignored in this layout.
            stmapAcquirePackedPair(stmapPathWanted, &left, &right);
        } else {
            left = stmapAcquire(stmapPathWanted);
        }
    }
    if (wantStmap && !packedLayout && !data->stmapRightPathWanted.empty()) {
        right = stmapAcquire(data->stmapRightPathWanted);
    }
    if (wantMetadata) {
        metadataClipPath = data->brawClipPathWanted;  // Manual Path source
#ifdef __APPLE__
        if (autoSource) {
            // Timeline (Auto): follow the playhead clip. The watcher keeps
            // pushing changes via timelineClipChanged; here we just take its
            // current answer so mode/size/mask edits apply immediately.
            ndi_timelinewatch::ensureStarted(&timelineClipChanged);
            metadataClipPath = ndi_timelinewatch::currentClipPath();
        }
#endif
        if (!metadataClipPath.empty()) {
            brawAcquireLensPair(metadataClipPath, brawMapSizeFromChoice(data->brawMapSizeChoice),
                                data->brawMaskChoice == 1, &left, &right);
        }
    }

    int status = kProjOff;
    bool stickyKeep = false;  // auto mode: hold the last camera's maps through gaps
    if (wantStmap) {
        if (!left && stmapPathWanted.empty()) {
            NDI_LOG("Equirect (STMap) selected but no STMap path is set — passthrough");
            status = kProjError;
        } else if (!left || !left->valid) {
            status = kProjError;
        } else if (right && !right->valid) {
            // An explicitly-set right map that fails to load: honest
            // passthrough beats silently warping that eye through the left map.
            status = kProjError;
        } else if (right && (right->map.width != left->map.width ||
                             right->map.height != left->map.height)) {
            NDI_LOG("STMap L/R size mismatch: left %dx%d vs right %dx%d — passthrough",
                    left->map.width, left->map.height, right->map.width, right->map.height);
            status = kProjMismatch;
        } else {
            status = kProjActive;
        }
    } else if (wantMetadata) {
        // The pair generates together from one calibration: either both eyes
        // are valid (same size by construction) or neither is.
        const bool pairValid = left && left->valid && right && right->valid;
        if (pairValid) {
            status = kProjActive;
        } else if (autoSource) {
            // Same sticky rule the watcher applies: an unusable playhead clip
            // (or none yet) keeps the last camera's maps instead of popping
            // the stream to passthrough.
            {
                std::lock_guard<std::mutex> lock(data->stmapMutex);
                stickyKeep = data->stmapLeft && data->stmapLeft->valid &&
                             data->stmapRight && data->stmapRight->valid;
            }
            if (stickyKeep) {
                status = kProjActive;
            } else {
                status = kProjError;
#ifdef __APPLE__
                std::string detail;
                ndi_timelinewatch::healthy(&detail);
                NDI_LOG_TEXT(("Equirect (Camera Metadata) auto: no usable clip under the "
                              "playhead yet (watcher: " + detail + ") — passthrough").c_str());
#endif
            }
        } else if (metadataClipPath.empty()) {
            NDI_LOG("Equirect (Camera Metadata) manual source but no camera clip is set — passthrough");
            status = kProjError;
        } else {
            status = kProjError;
        }
    }

#ifdef NDI_GPU_NATIVE
    // Pre-warm the GPU upload here, on the host's main thread, so the first
    // warped render doesn't pay it. The context's default device matches the
    // host queue's device except on multi-GPU machines, where the render path
    // uploads once itself (also outside the entry mutex). No GPU context yet
    // (NDI disabled) is fine — the render path covers it.
    if (status == kProjActive && !stickyKeep && left && left->valid &&
        data->gpuContext && data->gpuContext->initialized &&
        data->gpuContext->nativeContext) {
        stmapNativeBufferForQueue(left, data->gpuContext->nativeContext, nullptr);
        if (right) {
            stmapNativeBufferForQueue(right, data->gpuContext->nativeContext, nullptr);
        }
    }
#endif

    std::shared_ptr<StmapEntry> oldLeft, oldRight;
    if (!stickyKeep) {  // sticky: the maps already installed stay untouched
        std::lock_guard<std::mutex> lock(data->stmapMutex);
        oldLeft = std::move(data->stmapLeft);
        oldRight = std::move(data->stmapRight);
        data->stmapLeft = left;
        data->stmapRight = right;
        if (wantMetadata && autoSource) {
            data->autoLensAppliedPath = metadataClipPath;
            data->autoLensSkippedPath.clear();
        }
    }
    // oldLeft/oldRight die here — a replaced map's hundreds of MB free
    // outside the mutex the render path takes.
    data->projStatus.store(status, std::memory_order_relaxed);
}

static void setParamSecret(OfxParamHandle param, bool secret)
{
    if (!param) return;
    OfxPropertySetHandle props = NULL;
    gParamHost->paramGetPropertySet(param, &props);
    if (props) gPropHost->propSetInt(props, kOfxParamPropSecret, 0, secret ? 1 : 0);
}

// Show only the map-source fields the current mode actually reads: packed
// layout swaps the single STMap field in for the per-eye pair, and Timeline
// (Auto) hides the manual camera-clip picker. Called from createInstance (a
// saved project's choices restore their visibility) and instanceChanged.
// Resolve honors kOfxParamPropSecret edits on instance params.
static void updateParamVisibility(NDIInstanceData* data)
{
    const bool packed = (data->stmapLayout == 1);
    setParamSecret(data->stmapPackedParam, !packed);
    setParamSecret(data->stmapLeftParam, packed);
    setParamSecret(data->stmapRightParam, packed);
    const bool autoClip = (data->brawSourceChoice == 0);
    setParamSecret(data->brawClipParam, autoClip);
#ifdef NDI_HAS_BROWSE_DIALOGS
    setParamSecret(data->stmapPackedBrowseParam, !packed);
    setParamSecret(data->stmapLeftBrowseParam, packed);
    setParamSecret(data->stmapRightBrowseParam, packed);
    setParamSecret(data->brawClipBrowseParam, autoClip);
#endif
}

// Read every persisted parameter into the instance fields. Used at
// createInstance — so a saved project's values (source name above all) are
// honored BEFORE the first NDI attach, not only after the user touches a
// param — and at instanceChanged.
static void readInstanceParams(NDIInstanceData* myData)
{
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

    gParamHost->paramGetValue(myData->stereoPackingParam, &myData->stereoPacking);

    gParamHost->paramGetValue(myData->projectionParam, &myData->projectionMode);
    gParamHost->paramGetValue(myData->stmapLayoutParam, &myData->stmapLayout);
    char* stmapPackedPath = nullptr;
    gParamHost->paramGetValue(myData->stmapPackedParam, &stmapPackedPath);
    myData->stmapPackedPathWanted = stmapPackedPath ? stmapPackedPath : "";
    char* stmapLeftPath = nullptr;
    gParamHost->paramGetValue(myData->stmapLeftParam, &stmapLeftPath);
    myData->stmapLeftPathWanted = stmapLeftPath ? stmapLeftPath : "";
    char* stmapRightPath = nullptr;
    gParamHost->paramGetValue(myData->stmapRightParam, &stmapRightPath);
    myData->stmapRightPathWanted = stmapRightPath ? stmapRightPath : "";
    char* brawClipPath = nullptr;
    gParamHost->paramGetValue(myData->brawClipParam, &brawClipPath);
    myData->brawClipPathWanted = brawClipPath ? brawClipPath : "";
    gParamHost->paramGetValue(myData->brawSourceParam, &myData->brawSourceChoice);
    gParamHost->paramGetValue(myData->brawMapSizeParam, &myData->brawMapSizeChoice);
    gParamHost->paramGetValue(myData->brawMaskParam, &myData->brawMaskChoice);
    // Mirror the auto-mode config into the atomics the playhead watcher
    // thread reads (see applyAutoLensClip).
    myData->autoLensWanted.store(myData->projectionMode == 2 && myData->brawSourceChoice == 0,
                                 std::memory_order_relaxed);
    myData->autoLensMapSize.store(brawMapSizeFromChoice(myData->brawMapSizeChoice),
                                  std::memory_order_relaxed);
    myData->autoLensMask.store(myData->brawMaskChoice == 1, std::memory_order_relaxed);

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
}

static OfxStatus createInstance(OfxImageEffectHandle effect, OfxPropertySetHandle inArgs)
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
    myData->hub = nullptr;
    myData->ndiInitialized = false;
    myData->sourceName = "DaVinci Resolve NDI Output";
    myData->enabled = true;
    myData->frameRate = 30.0;
    myData->resolutionDivisor = 2;  // Half, mirroring the param default; render reads the param fresh

    // Stereo pairing context (issue #6)
    myData->renderEye = ndi_stereo::kEyeLeft;
    myData->renderTime = 0.0;
    myData->renderIsThumbnail = false;
    myData->stereoPacking = 0;
    myData->statusParamDirty = false;

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

    // Diagnostic probe state; the page is only handed over here, never at render time
    myData->resolvePage = "";
    myData->probeCallCount = 0;
    myData->probeHasLastCallTime = false;
    myData->timersEnabled = false;
    myData->timerBlitMs = myData->timerConvMs = 0.0;
    myData->timerFlushMs = myData->timerPackMs = myData->timerSendMs = 0.0;
#ifdef NDI_GPU_NATIVE
    myData->pump = nullptr;
#endif
    if (inArgs) {
        char* page = nullptr;
        if (gPropHost->propGetString(inArgs, kOfxImageEffectPropResolvePage, 0, &page) == kOfxStatOK && page) {
            myData->resolvePage = page;
        }
    }

    // Cache clip handles
    gEffectHost->clipGetHandle(effect, kOfxImageEffectSimpleSourceClipName, &myData->sourceClip, 0);
    gEffectHost->clipGetHandle(effect, kOfxImageEffectOutputClipName, &myData->outputClip, 0);

    // Cache parameter handles
    gParamHost->paramGetHandle(paramSet, kParamSourceName, &myData->sourceNameParam, 0);
    gParamHost->paramGetHandle(paramSet, kParamEnabled, &myData->enabledParam, 0);
    gParamHost->paramGetHandle(paramSet, kParamFrameRate, &myData->frameRateParam, 0);
    gParamHost->paramGetHandle(paramSet, kParamResolution, &myData->resolutionParam, 0);
    gParamHost->paramGetHandle(paramSet, kParamGPUAcceleration, &myData->gpuAccelerationParam, 0);
    gParamHost->paramGetHandle(paramSet, kParamAsyncSending, &myData->asyncSendingParam, 0);
    gParamHost->paramGetHandle(paramSet, kParamOptimalFormat, &myData->optimalFormatParam, 0);
    gParamHost->paramGetHandle(paramSet, kParamVersionLabel, &myData->versionLabelParam, 0);
    gParamHost->paramGetHandle(paramSet, kParamDebugLogging, &myData->debugLoggingParam, 0);
    gParamHost->paramGetHandle(paramSet, kParamStereoPacking, &myData->stereoPackingParam, 0);
    gParamHost->paramGetHandle(paramSet, kParamStereoStatus, &myData->stereoStatusParam, 0);
    gParamHost->paramGetHandle(paramSet, kParamProjection, &myData->projectionParam, 0);
    gParamHost->paramGetHandle(paramSet, kParamSTMapLayout, &myData->stmapLayoutParam, 0);
    gParamHost->paramGetHandle(paramSet, kParamSTMapPacked, &myData->stmapPackedParam, 0);
    gParamHost->paramGetHandle(paramSet, kParamSTMapLeft, &myData->stmapLeftParam, 0);
    gParamHost->paramGetHandle(paramSet, kParamSTMapRight, &myData->stmapRightParam, 0);
    gParamHost->paramGetHandle(paramSet, kParamBRAWSource, &myData->brawSourceParam, 0);
    gParamHost->paramGetHandle(paramSet, kParamBRAWClip, &myData->brawClipParam, 0);
#ifdef NDI_HAS_BROWSE_DIALOGS
    gParamHost->paramGetHandle(paramSet, kParamSTMapPackedBrowse, &myData->stmapPackedBrowseParam, 0);
    gParamHost->paramGetHandle(paramSet, kParamSTMapLeftBrowse, &myData->stmapLeftBrowseParam, 0);
    gParamHost->paramGetHandle(paramSet, kParamSTMapRightBrowse, &myData->stmapRightBrowseParam, 0);
    gParamHost->paramGetHandle(paramSet, kParamBRAWClipBrowse, &myData->brawClipBrowseParam, 0);
#endif
    gParamHost->paramGetHandle(paramSet, kParamBRAWMapSize, &myData->brawMapSizeParam, 0);
    gParamHost->paramGetHandle(paramSet, kParamBRAWMask, &myData->brawMaskParam, 0);
    gParamHost->paramGetHandle(paramSet, kParamHDREnabled, &myData->hdrEnabledParam, 0);
    gParamHost->paramGetHandle(paramSet, kParamColorSpace, &myData->colorSpaceParam, 0);
    gParamHost->paramGetHandle(paramSet, kParamTransferFunction, &myData->transferFunctionParam, 0);
    gParamHost->paramGetHandle(paramSet, kParamMaxCLL, &myData->maxCLLParam, 0);
    gParamHost->paramGetHandle(paramSet, kParamMaxFALL, &myData->maxFALLParam, 0);

    // Set instance data
    gPropHost->propSetPointer(effectProps, kOfxPropInstanceData, 0, (void *) myData);

    // Honor the project's saved parameter values (source name above all)
    // before the first NDI attach.
    readInstanceParams(myData);
    updateParamVisibility(myData);

    // Auto camera-clip mode: visible to the playhead watcher from here on
    // (the walk holds the registry mutex, so destroyInstance can't race it).
    instanceRegistryAdd(myData);

    // Initialize NDI if enabled
    if (myData->enabled) {
        initializeNDI(myData);
    }

    // After NDI init so the GPU context exists for the STMap pre-warm — a
    // saved project's Equirect mode loads (and uploads) its maps now.
    refreshSTMaps(myData);

    NDI_LOG_TEXT(("Instance created successfully on page '" +
                  (myData->resolvePage.empty() ? std::string("?") : myData->resolvePage) + "'").c_str());
    return kOfxStatOK;
}

static OfxStatus destroyInstance(OfxImageEffectHandle effect)
{
    NDI_LOG("Destroying instance");

    NDIInstanceData *myData = getInstanceData(effect);
    if (myData) {
        instanceRegistryRemove(myData);  // before delete: the watcher walk holds the same mutex
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

#ifdef NDI_HAS_BROWSE_DIALOGS
        // Browse buttons: pop the native open dialog and write the picked path
        // into the matching STMap field, then fall through — the reads below
        // pick the new value up and refreshSTMaps loads the map. Cancel (or
        // any dialog failure) changes nothing.
        if (strcmp(paramName, kParamSTMapPackedBrowse) == 0 ||
            strcmp(paramName, kParamSTMapLeftBrowse) == 0 ||
            strcmp(paramName, kParamSTMapRightBrowse) == 0) {
            const bool isPacked = (strcmp(paramName, kParamSTMapPackedBrowse) == 0);
            const bool isLeft = (strcmp(paramName, kParamSTMapLeftBrowse) == 0);
            OfxParamHandle pathParam = isPacked ? myData->stmapPackedParam
                                     : isLeft  ? myData->stmapLeftParam : myData->stmapRightParam;
            char* currentPath = nullptr;
            gParamHost->paramGetValue(pathParam, &currentPath);
            char picked[4096];
            if (native_open_file_dialog(isPacked ? "Choose the packed side-by-side STMap EXR"
                                    : isLeft  ? "Choose the left-eye STMap EXR"
                                              : "Choose the right-eye STMap EXR",
                                     "exr", currentPath, picked, sizeof(picked))) {
                gParamHost->paramSetValue(pathParam, picked);
                NDI_LOG_TEXT((std::string("STMap browse picked: '") + picked + "'").c_str());
            }
        }
        if (strcmp(paramName, kParamBRAWClipBrowse) == 0) {
            char* currentPath = nullptr;
            gParamHost->paramGetValue(myData->brawClipParam, &currentPath);
            char picked[4096];
            if (native_open_file_dialog("Choose any BRAW clip shot on the URSA Cine Immersive",
                                     "braw", currentPath, picked, sizeof(picked))) {
                gParamHost->paramSetValue(myData->brawClipParam, picked);
                NDI_LOG_TEXT((std::string("Camera clip browse picked: '") + picked + "'").c_str());
            }
        }
#endif

        readInstanceParams(myData);
        updateParamVisibility(myData);
        refreshSTMaps(myData);

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

    int optimalFormat;
    gParamHost->paramGetValue(myData->optimalFormatParam, &optimalFormat);
    myData->optimalFormat = (optimalFormat != 0);

    int asyncSending;
    gParamHost->paramGetValue(myData->asyncSendingParam, &asyncSending);
    myData->asyncSending = (asyncSending != 0);

    int resolutionChoice = 0;
    gParamHost->paramGetValue(myData->resolutionParam, &resolutionChoice);
    myData->resolutionDivisor = ndi_stream::divisorForResolutionChoice(resolutionChoice);

    gParamHost->paramGetValue(myData->stereoPackingParam, &myData->stereoPacking);

    // Log current parameter state for debugging
    NDI_LOG("Render params - enabled=%d, hdr=%d, gpu=%d, divisor=%d",
           myData->enabled, myData->hdrEnabled, myData->gpuAcceleration, myData->resolutionDivisor);

    // Get time
    double time;
    gPropHost->propGetDouble(inArgs, kOfxPropTime, 0, &time);

    // Get render window
    OfxRectI renderWindow;
    gPropHost->propGetIntN(inArgs, kOfxImageEffectPropRenderWindow, 4, &renderWindow.x1);

    // Stereo pairing context (issue #6): which eye this call renders, at what
    // frame time, and whether it's a filmstrip thumbnail. Read every call —
    // the pairer keys on these.
    int eyeValue = 0;
    const bool hasEye =
        (gPropHost->propGetInt(inArgs, kOfxImageEffectPropEyeToRender, 0, &eyeValue) == kOfxStatOK);
    myData->renderEye = (hasEye && eyeValue == kOfxImageEyeRight) ? ndi_stereo::kEyeRight
                                                                  : ndi_stereo::kEyeLeft;
    myData->renderTime = time;

    OfxPropertySetHandle srcClipProps = NULL;
    int thumbnailValue = 0;
    bool hasThumbnail = false;
    if (gEffectHost->clipGetPropertySet(myData->sourceClip, &srcClipProps) == kOfxStatOK && srcClipProps) {
        hasThumbnail =
            (gPropHost->propGetInt(srcClipProps, kOfxImageClipPropThumbnail, 0, &thumbnailValue) == kOfxStatOK);
    }
    myData->renderIsThumbnail = (hasThumbnail && thumbnailValue != 0);

    // Select this render's warp map (issue #7): only when Equirect is chosen
    // (read fresh, like every param) and the last refresh validated the maps.
    // Thumbnails stay passthrough — a filmstrip thumb warped to map
    // dimensions would emit giant frames. Copying the shared_ptr under the
    // lock keeps the entry alive for this whole render even if a parameter
    // change swaps the maps mid-flight.
    int projectionChoice = 0;
    gParamHost->paramGetValue(myData->projectionParam, &projectionChoice);
    myData->renderStmap.reset();
    if ((projectionChoice == 1 || projectionChoice == 2) && !myData->renderIsThumbnail &&
        myData->projStatus.load(std::memory_order_relaxed) == kProjActive) {
        std::lock_guard<std::mutex> lock(myData->stmapMutex);
        const std::shared_ptr<StmapEntry>& sel =
            (myData->renderEye == ndi_stereo::kEyeRight && myData->stmapRight && myData->stmapRight->valid)
                ? myData->stmapRight
                : myData->stmapLeft;
        if (sel && sel->valid) {
            myData->renderStmap = sel;
        }
    }

    // Diagnostic render-call probe: when enabled, log exactly what the host feeds
    // this render call — before any image fetch, so calls that fail later still show.
    int debugLogging = 0;
    gParamHost->paramGetValue(myData->debugLoggingParam, &debugLogging);
    if (debugLogging) {
        ndi_probe::ProbeRenderInfo info;
        info.page = myData->resolvePage.c_str();
        info.time = time;
        info.width = renderWindow.x2 - renderWindow.x1;
        info.height = renderWindow.y2 - renderWindow.y1;

        info.hasEye = hasEye;
        info.eye = eyeValue;

        int srcFrame = 0;
        info.hasSrcFrame = (gPropHost->propGetInt(inArgs, kOfxImageEffectPropSrcFrame, 0, &srcFrame) == kOfxStatOK);
        info.srcFrame = srcFrame;

        double scale[2] = {0.0, 0.0};
        info.hasScale = (gPropHost->propGetDoubleN(inArgs, kOfxImageEffectPropRenderScale, 2, scale) == kOfxStatOK);
        info.scaleX = scale[0];
        info.scaleY = scale[1];

        info.hasThumbnail = hasThumbnail;
        info.thumbnail = thumbnailValue;

        {
            std::lock_guard<std::mutex> lock(myData->probeMutex);
            auto now = std::chrono::steady_clock::now();
            info.callIndex = ++myData->probeCallCount;
            if (myData->probeHasLastCallTime) {
                info.hasDt = true;
                info.dtMs = std::chrono::duration<double, std::milli>(now - myData->probeLastCallTime).count();
            }
            myData->probeLastCallTime = now;
            myData->probeHasLastCallTime = true;
        }

        NDI_LOG_TEXT(ndi_probe::formatProbeLine(info).c_str());
    }

    // Stage timers for the mtimer diagnostic line (issue #5): reset per render.
    myData->timersEnabled = (debugLogging != 0);
    myData->timerBlitMs = myData->timerConvMs = 0.0;
    myData->timerFlushMs = myData->timerPackMs = myData->timerSendMs = 0.0;
    const auto renderT0 = std::chrono::steady_clock::now();

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
    const double imagesMs = msSince(renderT0);

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

#ifdef NDI_GPU_NATIVE
    // GPU render: describe() declared Metal/CUDA render support, so when the
    // host sets the matching Enabled property the image data pointers are
    // device buffers, not CPU memory. The queue/stream pointer is the host's
    // own — work enqueued there is ordered after the host's renders.
    int gpuEnabled = 0;
    void* gpuQueue = nullptr;
#ifdef __APPLE__
    gPropHost->propGetInt(inArgs, kOfxImageEffectPropMetalEnabled, 0, &gpuEnabled);
    if (gpuEnabled) {
        gPropHost->propGetPointer(inArgs, kOfxImageEffectPropMetalCommandQueue, 0, &gpuQueue);
    }
#else
    gPropHost->propGetInt(inArgs, kOfxImageEffectPropCudaEnabled, 0, &gpuEnabled);
    if (gpuEnabled) {
        // NULL when the host predates CudaStreamSupported: the module then
        // runs on its own stream (the host synchronizes around render in
        // that mode).
        gPropHost->propGetPointer(inArgs, kOfxImageEffectPropCudaStream, 0, &gpuQueue);
    }
#endif
    if (gpuEnabled) {
        OfxStatus gpuStatus = renderGPUFrame(myData, srcData, dstData, width, height,
                                             srcRowBytes, dstRowBytes, gpuQueue);
        gEffectHost->clipReleaseImage(sourceImg);
        gEffectHost->clipReleaseImage(outputImg);
        flushStatusParam(myData);
        if (myData->timersEnabled) {
            // All-numeric on purpose — dynamic %s strings get redacted to
            // <private> by unified logging (LEARNINGS 2026-08-28). eye: 0=L 1=R.
            NDI_LOG("mtimer eye=%d total=%.1f images=%.1f blit=%.1f conv=%.1f flush=%.1f pack=%.1f send=%.1f",
                    myData->renderEye == ndi_stereo::kEyeRight ? 1 : 0,
                    msSince(renderT0), imagesMs, myData->timerBlitMs, myData->timerConvMs,
                    myData->timerFlushMs, myData->timerPackMs, myData->timerSendMs);
        }
        NDI_LOG("Render completed (" NDI_GPU_BACKEND_NAME ")");
        return gpuStatus;
    }
#endif

    if (srcData && dstData) {
        // Simple copy for float RGBA
        memcpy(dstData, srcData, height * dstRowBytes);

        // Send to NDI (downscale + vertical flip handled inside)
        sendCPUFrameToNDI(myData, srcData, width, height, srcRowBytes);
    }

    // Release images
    gEffectHost->clipReleaseImage(sourceImg);
    gEffectHost->clipReleaseImage(outputImg);

    flushStatusParam(myData);
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

#ifdef __APPLE__
    // GPU fast path (issue #5): accept frames as Metal buffers so the
    // downscale+convert runs before any readback. CPU rendering stays
    // supported — the host chooses per render via kOfxImageEffectPropMetalEnabled.
    gPropHost->propSetString(props, kOfxImageEffectPropMetalRenderSupported, 0, "true");
#elif defined(NDI_HAS_CUDA)
    // CUDA render + CUDA-stream support, Windows only (ticket #22, spec
    // decision 4): accept frames as CUDA device buffers, with the host's
    // stream handed per render via kOfxImageEffectPropCudaStream. CPU
    // rendering stays supported — non-NVIDIA machines keep receiving CPU
    // buffers and the existing CPU path.
    gPropHost->propSetString(props, kOfxImageEffectPropCudaRenderSupported, 0, "true");
    gPropHost->propSetString(props, kOfxImageEffectPropCudaStreamSupported, 0, "true");
#endif

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

    OfxPropertySetHandle stereoGroupProps = NULL;
    gParamHost->paramDefine(paramSet, kOfxParamTypeGroup, "stereoGroup", &stereoGroupProps);
    gPropHost->propSetString(stereoGroupProps, kOfxPropLabel, 0, "Stereo");
    gPropHost->propSetInt(stereoGroupProps, kOfxParamPropGroupOpen, 0, 1); // Open by default

    OfxPropertySetHandle projectionGroupProps = NULL;
    gParamHost->paramDefine(paramSet, kOfxParamTypeGroup, "projectionGroup", &projectionGroupProps);
    gPropHost->propSetString(projectionGroupProps, kOfxPropLabel, 0, "Projection");
    gPropHost->propSetInt(projectionGroupProps, kOfxParamPropGroupOpen, 0, 1); // Open by default

    OfxPropertySetHandle performanceGroupProps = NULL;
    gParamHost->paramDefine(paramSet, kOfxParamTypeGroup, "performanceGroup", &performanceGroupProps);
    gPropHost->propSetString(performanceGroupProps, kOfxPropLabel, 0, "Performance Settings");
    gPropHost->propSetInt(performanceGroupProps, kOfxParamPropGroupOpen, 0, 1); // Open by default

    OfxPropertySetHandle hdrGroupProps = NULL;
    gParamHost->paramDefine(paramSet, kOfxParamTypeGroup, "hdrGroup", &hdrGroupProps);
    gPropHost->propSetString(hdrGroupProps, kOfxPropLabel, 0, "HDR Settings");
    gPropHost->propSetInt(hdrGroupProps, kOfxParamPropGroupOpen, 0, 0); // Closed by default

    OfxPropertySetHandle diagnosticsGroupProps = NULL;
    gParamHost->paramDefine(paramSet, kOfxParamTypeGroup, "diagnosticsGroup", &diagnosticsGroupProps);
    gPropHost->propSetString(diagnosticsGroupProps, kOfxPropLabel, 0, "Diagnostics");
    gPropHost->propSetInt(diagnosticsGroupProps, kOfxParamPropGroupOpen, 0, 0); // Closed by default

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
    gPropHost->propSetDouble(frameRateProps, kOfxParamPropDefault, 0, 30.0);
    gPropHost->propSetDouble(frameRateProps, kOfxParamPropMin, 0, 1.0);
    gPropHost->propSetDouble(frameRateProps, kOfxParamPropMax, 0, 120.0);
    gPropHost->propSetDouble(frameRateProps, kOfxParamPropDisplayMin, 0, 23.976);
    gPropHost->propSetDouble(frameRateProps, kOfxParamPropDisplayMax, 0, 60.0);
    gPropHost->propSetInt(frameRateProps, kOfxParamPropAnimates, 0, 0);
    gPropHost->propSetString(frameRateProps, kOfxParamPropParent, 0, "basicGroup");

    // Define stream resolution parameter - in Basic group
    OfxPropertySetHandle resolutionProps = NULL;
    gParamHost->paramDefine(paramSet, kOfxParamTypeChoice, kParamResolution, &resolutionProps);
    gPropHost->propSetString(resolutionProps, kOfxPropLabel, 0, kParamResolutionLabel);
    gPropHost->propSetString(resolutionProps, kOfxParamPropScriptName, 0, kParamResolution);
    gPropHost->propSetString(resolutionProps, kOfxParamPropHint, 0, kParamResolutionHint);
    gPropHost->propSetString(resolutionProps, kOfxParamPropChoiceOption, 0, "Full");
    gPropHost->propSetString(resolutionProps, kOfxParamPropChoiceOption, 1, "Half");
    gPropHost->propSetString(resolutionProps, kOfxParamPropChoiceOption, 2, "Quarter");
    gPropHost->propSetInt(resolutionProps, kOfxParamPropDefault, 0, 1); // Half
    gPropHost->propSetInt(resolutionProps, kOfxParamPropAnimates, 0, 0);
    gPropHost->propSetString(resolutionProps, kOfxParamPropParent, 0, "basicGroup");

    // Define stereo packing parameter - in Stereo group (issue #6)
    OfxPropertySetHandle stereoPackingProps = NULL;
    gParamHost->paramDefine(paramSet, kOfxParamTypeChoice, kParamStereoPacking, &stereoPackingProps);
    gPropHost->propSetString(stereoPackingProps, kOfxPropLabel, 0, kParamStereoPackingLabel);
    gPropHost->propSetString(stereoPackingProps, kOfxParamPropScriptName, 0, kParamStereoPacking);
    gPropHost->propSetString(stereoPackingProps, kOfxParamPropHint, 0, kParamStereoPackingHint);
    gPropHost->propSetString(stereoPackingProps, kOfxParamPropChoiceOption, 0, "Side-by-Side");
    gPropHost->propSetString(stereoPackingProps, kOfxParamPropChoiceOption, 1, "Top-Bottom");
    gPropHost->propSetInt(stereoPackingProps, kOfxParamPropDefault, 0, 0); // Side-by-Side
    gPropHost->propSetInt(stereoPackingProps, kOfxParamPropAnimates, 0, 0);
    gPropHost->propSetString(stereoPackingProps, kOfxParamPropParent, 0, "stereoGroup");

    // Define stream status parameter (informational display) - in Stereo group
    OfxPropertySetHandle stereoStatusProps = NULL;
    gParamHost->paramDefine(paramSet, kOfxParamTypeString, kParamStereoStatus, &stereoStatusProps);
    gPropHost->propSetString(stereoStatusProps, kOfxPropLabel, 0, kParamStereoStatusLabel);
    gPropHost->propSetString(stereoStatusProps, kOfxParamPropScriptName, 0, kParamStereoStatus);
    gPropHost->propSetString(stereoStatusProps, kOfxParamPropHint, 0, kParamStereoStatusHint);
    gPropHost->propSetString(stereoStatusProps, kOfxParamPropDefault, 0, "Mono");
    gPropHost->propSetInt(stereoStatusProps, kOfxParamPropAnimates, 0, 0);
    gPropHost->propSetInt(stereoStatusProps, kOfxParamPropPersistant, 0, 0); // live status, not a setting
    gPropHost->propSetString(stereoStatusProps, kOfxParamPropParent, 0, "stereoGroup");

    // Define projection mode parameter - in Projection group (issue #7)
    OfxPropertySetHandle projectionProps = NULL;
    gParamHost->paramDefine(paramSet, kOfxParamTypeChoice, kParamProjection, &projectionProps);
    gPropHost->propSetString(projectionProps, kOfxPropLabel, 0, kParamProjectionLabel);
    gPropHost->propSetString(projectionProps, kOfxParamPropScriptName, 0, kParamProjection);
    gPropHost->propSetString(projectionProps, kOfxParamPropHint, 0, kParamProjectionHint);
    gPropHost->propSetString(projectionProps, kOfxParamPropChoiceOption, 0, "Passthrough");
    gPropHost->propSetString(projectionProps, kOfxParamPropChoiceOption, 1, "Equirect (STMap)");
    gPropHost->propSetString(projectionProps, kOfxParamPropChoiceOption, 2, "Equirect (Camera Metadata)");
    gPropHost->propSetInt(projectionProps, kOfxParamPropDefault, 0, 0); // Passthrough
    gPropHost->propSetInt(projectionProps, kOfxParamPropAnimates, 0, 0);
    gPropHost->propSetString(projectionProps, kOfxParamPropParent, 0, "projectionGroup");

    // Define STMap layout parameter - in Projection group
    OfxPropertySetHandle stmapLayoutProps = NULL;
    gParamHost->paramDefine(paramSet, kOfxParamTypeChoice, kParamSTMapLayout, &stmapLayoutProps);
    gPropHost->propSetString(stmapLayoutProps, kOfxPropLabel, 0, kParamSTMapLayoutLabel);
    gPropHost->propSetString(stmapLayoutProps, kOfxParamPropScriptName, 0, kParamSTMapLayout);
    gPropHost->propSetString(stmapLayoutProps, kOfxParamPropHint, 0, kParamSTMapLayoutHint);
    gPropHost->propSetString(stmapLayoutProps, kOfxParamPropChoiceOption, 0, "Per-Eye Files");
    gPropHost->propSetString(stmapLayoutProps, kOfxParamPropChoiceOption, 1, "Packed Side-by-Side");
    gPropHost->propSetInt(stmapLayoutProps, kOfxParamPropDefault, 0, 1); // Packed Side-by-Side
    gPropHost->propSetInt(stmapLayoutProps, kOfxParamPropAnimates, 0, 0);
    gPropHost->propSetString(stmapLayoutProps, kOfxParamPropParent, 0, "projectionGroup");

    // The STMap path fields swap with the layout choice (updateParamVisibility):
    // packed shows the single field below, per-eye shows the left/right pair.
    // Describe-time secret states match the Packed Side-by-Side default;
    // createInstance re-syncs them to a saved project's choice.

    // Define packed side-by-side STMap path parameter - in Projection group
    OfxPropertySetHandle stmapPackedProps = NULL;
    gParamHost->paramDefine(paramSet, kOfxParamTypeString, kParamSTMapPacked, &stmapPackedProps);
    gPropHost->propSetString(stmapPackedProps, kOfxPropLabel, 0, kParamSTMapPackedLabel);
    gPropHost->propSetString(stmapPackedProps, kOfxParamPropScriptName, 0, kParamSTMapPacked);
    gPropHost->propSetString(stmapPackedProps, kOfxParamPropHint, 0, kParamSTMapPackedHint);
    gPropHost->propSetString(stmapPackedProps, kOfxParamPropDefault, 0, "");
    // FilePathExists stays at its spec default (1): hosts that render a
    // picker for filePath strings show an open-existing dialog.
    gPropHost->propSetString(stmapPackedProps, kOfxParamPropStringMode, 0, kOfxParamStringIsFilePath);
    gPropHost->propSetInt(stmapPackedProps, kOfxParamPropAnimates, 0, 0);
    gPropHost->propSetString(stmapPackedProps, kOfxParamPropParent, 0, "projectionGroup");

#ifdef NDI_HAS_BROWSE_DIALOGS
    // Define the packed STMap browse button - in Projection group (native
    // dialog; Resolve draws no browse control on filePath string params)
    OfxPropertySetHandle stmapPackedBrowseProps = NULL;
    gParamHost->paramDefine(paramSet, kOfxParamTypePushButton, kParamSTMapPackedBrowse, &stmapPackedBrowseProps);
    gPropHost->propSetString(stmapPackedBrowseProps, kOfxPropLabel, 0, kParamSTMapPackedBrowseLabel);
    gPropHost->propSetString(stmapPackedBrowseProps, kOfxParamPropScriptName, 0, kParamSTMapPackedBrowse);
    gPropHost->propSetString(stmapPackedBrowseProps, kOfxParamPropHint, 0, kParamSTMapPackedBrowseHint);
    gPropHost->propSetString(stmapPackedBrowseProps, kOfxParamPropParent, 0, "projectionGroup");
#endif

    // Define left-eye STMap path parameter - in Projection group
    OfxPropertySetHandle stmapLeftProps = NULL;
    gParamHost->paramDefine(paramSet, kOfxParamTypeString, kParamSTMapLeft, &stmapLeftProps);
    gPropHost->propSetString(stmapLeftProps, kOfxPropLabel, 0, kParamSTMapLeftLabel);
    gPropHost->propSetString(stmapLeftProps, kOfxParamPropScriptName, 0, kParamSTMapLeft);
    gPropHost->propSetString(stmapLeftProps, kOfxParamPropHint, 0, kParamSTMapLeftHint);
    gPropHost->propSetString(stmapLeftProps, kOfxParamPropDefault, 0, "");
    gPropHost->propSetString(stmapLeftProps, kOfxParamPropStringMode, 0, kOfxParamStringIsFilePath);
    gPropHost->propSetInt(stmapLeftProps, kOfxParamPropAnimates, 0, 0);
    gPropHost->propSetInt(stmapLeftProps, kOfxParamPropSecret, 0, 1); // hidden in packed layout (the default)
    gPropHost->propSetString(stmapLeftProps, kOfxParamPropParent, 0, "projectionGroup");

#ifdef NDI_HAS_BROWSE_DIALOGS
    // Define the left-eye browse button - in Projection group
    OfxPropertySetHandle stmapLeftBrowseProps = NULL;
    gParamHost->paramDefine(paramSet, kOfxParamTypePushButton, kParamSTMapLeftBrowse, &stmapLeftBrowseProps);
    gPropHost->propSetString(stmapLeftBrowseProps, kOfxPropLabel, 0, kParamSTMapLeftBrowseLabel);
    gPropHost->propSetString(stmapLeftBrowseProps, kOfxParamPropScriptName, 0, kParamSTMapLeftBrowse);
    gPropHost->propSetString(stmapLeftBrowseProps, kOfxParamPropHint, 0, kParamSTMapLeftBrowseHint);
    gPropHost->propSetInt(stmapLeftBrowseProps, kOfxParamPropSecret, 0, 1);
    gPropHost->propSetString(stmapLeftBrowseProps, kOfxParamPropParent, 0, "projectionGroup");
#endif

    // Define right-eye STMap path parameter - in Projection group
    OfxPropertySetHandle stmapRightProps = NULL;
    gParamHost->paramDefine(paramSet, kOfxParamTypeString, kParamSTMapRight, &stmapRightProps);
    gPropHost->propSetString(stmapRightProps, kOfxPropLabel, 0, kParamSTMapRightLabel);
    gPropHost->propSetString(stmapRightProps, kOfxParamPropScriptName, 0, kParamSTMapRight);
    gPropHost->propSetString(stmapRightProps, kOfxParamPropHint, 0, kParamSTMapRightHint);
    gPropHost->propSetString(stmapRightProps, kOfxParamPropDefault, 0, "");
    gPropHost->propSetString(stmapRightProps, kOfxParamPropStringMode, 0, kOfxParamStringIsFilePath);
    gPropHost->propSetInt(stmapRightProps, kOfxParamPropAnimates, 0, 0);
    gPropHost->propSetInt(stmapRightProps, kOfxParamPropSecret, 0, 1); // hidden in packed layout (the default)
    gPropHost->propSetString(stmapRightProps, kOfxParamPropParent, 0, "projectionGroup");

#ifdef NDI_HAS_BROWSE_DIALOGS
    // Define the right-eye browse button - in Projection group
    OfxPropertySetHandle stmapRightBrowseProps = NULL;
    gParamHost->paramDefine(paramSet, kOfxParamTypePushButton, kParamSTMapRightBrowse, &stmapRightBrowseProps);
    gPropHost->propSetString(stmapRightBrowseProps, kOfxPropLabel, 0, kParamSTMapRightBrowseLabel);
    gPropHost->propSetString(stmapRightBrowseProps, kOfxParamPropScriptName, 0, kParamSTMapRightBrowse);
    gPropHost->propSetString(stmapRightBrowseProps, kOfxParamPropHint, 0, kParamSTMapRightBrowseHint);
    gPropHost->propSetInt(stmapRightBrowseProps, kOfxParamPropSecret, 0, 1);
    gPropHost->propSetString(stmapRightBrowseProps, kOfxParamPropParent, 0, "projectionGroup");
#endif

    // Define the camera clip source parameter - in Projection group (issue
    // #11: Timeline (Auto) follows the playhead via the scripting watcher,
    // Manual Path reads the clip picked below)
    OfxPropertySetHandle brawSourceProps = NULL;
    gParamHost->paramDefine(paramSet, kOfxParamTypeChoice, kParamBRAWSource, &brawSourceProps);
    gPropHost->propSetString(brawSourceProps, kOfxPropLabel, 0, kParamBRAWSourceLabel);
    gPropHost->propSetString(brawSourceProps, kOfxParamPropScriptName, 0, kParamBRAWSource);
    gPropHost->propSetString(brawSourceProps, kOfxParamPropHint, 0, kParamBRAWSourceHint);
    gPropHost->propSetString(brawSourceProps, kOfxParamPropChoiceOption, 0, "Timeline (Auto)");
    gPropHost->propSetString(brawSourceProps, kOfxParamPropChoiceOption, 1, "Manual Path");
    gPropHost->propSetInt(brawSourceProps, kOfxParamPropDefault, 0, 0); // Timeline (Auto)
    gPropHost->propSetInt(brawSourceProps, kOfxParamPropAnimates, 0, 0);
    gPropHost->propSetString(brawSourceProps, kOfxParamPropParent, 0, "projectionGroup");

    // Define the camera clip path parameter - in Projection group (issue #11:
    // Manual Path mode reads the embedded lens calibration from any BRAW
    // clip shot on the camera)
    OfxPropertySetHandle brawClipProps = NULL;
    gParamHost->paramDefine(paramSet, kOfxParamTypeString, kParamBRAWClip, &brawClipProps);
    gPropHost->propSetString(brawClipProps, kOfxPropLabel, 0, kParamBRAWClipLabel);
    gPropHost->propSetString(brawClipProps, kOfxParamPropScriptName, 0, kParamBRAWClip);
    gPropHost->propSetString(brawClipProps, kOfxParamPropHint, 0, kParamBRAWClipHint);
    gPropHost->propSetString(brawClipProps, kOfxParamPropDefault, 0, "");
    gPropHost->propSetString(brawClipProps, kOfxParamPropStringMode, 0, kOfxParamStringIsFilePath);
    gPropHost->propSetInt(brawClipProps, kOfxParamPropAnimates, 0, 0);
    // Hidden while Camera Clip Source sits at Timeline (Auto), the default —
    // updateParamVisibility shows it when Manual Path is chosen.
    gPropHost->propSetInt(brawClipProps, kOfxParamPropSecret, 0, 1);
    gPropHost->propSetString(brawClipProps, kOfxParamPropParent, 0, "projectionGroup");

#ifdef NDI_HAS_BROWSE_DIALOGS
    // Define the camera clip browse button - in Projection group
    OfxPropertySetHandle brawClipBrowseProps = NULL;
    gParamHost->paramDefine(paramSet, kOfxParamTypePushButton, kParamBRAWClipBrowse, &brawClipBrowseProps);
    gPropHost->propSetString(brawClipBrowseProps, kOfxPropLabel, 0, kParamBRAWClipBrowseLabel);
    gPropHost->propSetString(brawClipBrowseProps, kOfxParamPropScriptName, 0, kParamBRAWClipBrowse);
    gPropHost->propSetString(brawClipBrowseProps, kOfxParamPropHint, 0, kParamBRAWClipBrowseHint);
    gPropHost->propSetInt(brawClipBrowseProps, kOfxParamPropSecret, 0, 1); // hidden in Timeline (Auto), the default
    gPropHost->propSetString(brawClipBrowseProps, kOfxParamPropParent, 0, "projectionGroup");
#endif

    // Define the metadata map size parameter - in Projection group
    OfxPropertySetHandle brawMapSizeProps = NULL;
    gParamHost->paramDefine(paramSet, kOfxParamTypeChoice, kParamBRAWMapSize, &brawMapSizeProps);
    gPropHost->propSetString(brawMapSizeProps, kOfxPropLabel, 0, kParamBRAWMapSizeLabel);
    gPropHost->propSetString(brawMapSizeProps, kOfxParamPropScriptName, 0, kParamBRAWMapSize);
    gPropHost->propSetString(brawMapSizeProps, kOfxParamPropHint, 0, kParamBRAWMapSizeHint);
    gPropHost->propSetString(brawMapSizeProps, kOfxParamPropChoiceOption, 0, "1024 x 1024 per eye");
    gPropHost->propSetString(brawMapSizeProps, kOfxParamPropChoiceOption, 1, "2048 x 2048 per eye");
    gPropHost->propSetString(brawMapSizeProps, kOfxParamPropChoiceOption, 2, "4096 x 4096 per eye");
    gPropHost->propSetInt(brawMapSizeProps, kOfxParamPropDefault, 0, 1); // 2048
    gPropHost->propSetInt(brawMapSizeProps, kOfxParamPropAnimates, 0, 0);
    gPropHost->propSetString(brawMapSizeProps, kOfxParamPropParent, 0, "projectionGroup");

    // Define the metadata edge mask parameter - in Projection group
    OfxPropertySetHandle brawMaskProps = NULL;
    gParamHost->paramDefine(paramSet, kOfxParamTypeChoice, kParamBRAWMask, &brawMaskProps);
    gPropHost->propSetString(brawMaskProps, kOfxPropLabel, 0, kParamBRAWMaskLabel);
    gPropHost->propSetString(brawMaskProps, kOfxParamPropScriptName, 0, kParamBRAWMask);
    gPropHost->propSetString(brawMaskProps, kOfxParamPropHint, 0, kParamBRAWMaskHint);
    gPropHost->propSetString(brawMaskProps, kOfxParamPropChoiceOption, 0, "Off");
    gPropHost->propSetString(brawMaskProps, kOfxParamPropChoiceOption, 1, "Camera");
    gPropHost->propSetInt(brawMaskProps, kOfxParamPropDefault, 0, 0); // Off
    gPropHost->propSetInt(brawMaskProps, kOfxParamPropAnimates, 0, 0);
    gPropHost->propSetString(brawMaskProps, kOfxParamPropParent, 0, "projectionGroup");

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

    // Define debug logging parameter - in Diagnostics group
    OfxPropertySetHandle debugLoggingProps = NULL;
    gParamHost->paramDefine(paramSet, kOfxParamTypeBoolean, kParamDebugLogging, &debugLoggingProps);
    gPropHost->propSetString(debugLoggingProps, kOfxPropLabel, 0, kParamDebugLoggingLabel);
    gPropHost->propSetString(debugLoggingProps, kOfxParamPropScriptName, 0, kParamDebugLogging);
    gPropHost->propSetString(debugLoggingProps, kOfxParamPropHint, 0, kParamDebugLoggingHint);
    gPropHost->propSetInt(debugLoggingProps, kOfxParamPropDefault, 0, 0); // Default to disabled
    gPropHost->propSetInt(debugLoggingProps, kOfxParamPropAnimates, 0, 0);
    gPropHost->propSetString(debugLoggingProps, kOfxParamPropParent, 0, "diagnosticsGroup");

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
            return createInstance((OfxImageEffectHandle) handle, inArgs);
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