# Feasibility: A Windows Build + Installer for the NDI Output OFX Plugin

**Date:** 2026-08-30 · **Researched on:** this repo @ v1.14.0 (`584501c`) and the LSVR post machine's primary sources on disk (Resolve 21.0.4 developer kit, README dated 2026-05-12; NDI Advanced SDK 6.2.0 docs + license; Blackmagic RAW SDK 5.1 incl. its `Win/` variant), plus vendor web documentation cited inline.
**Question:** Can we ship a Windows version of `NDIOutput.ofx` — GPU path, stereo pairing, STMap/metadata warp, HDR — with a real installer, and what exactly does it take?

**Verdict: Yes — feasible, no hard blockers found, but it is a real port, not a build fix.** The May-2025 scaffold (commit `50eacc1`) predates the plugin's entire modern architecture and its 460-line CUDA file is a host-memory sketch of a design the codebase abandoned at v1.5 — plan a restart on good bones, not a repair. The bones are genuinely good: everything that makes this plugin interesting (stereo pairing, canvas guard, STMap/lens-map math, the sender hub, the async-pump design) is already platform-neutral C++, and the OFX CUDA-stream contract ("enqueue on the host's stream, never block, never sync") is a word-for-word match for the Metal architecture — the port is a translation. Three loads of work: **(1)** a CUDA twin of the fused GPU-native pipeline (~6 kernels + submit ring, the core effort — and effectively mandatory, because the CPU-buffer fallback that works fine at HD/UHD collapses at 8K stereo exactly as macOS did before issue #5); **(2)** bounded Windows plumbing (logging, DLL delay-loading, zlib, UTF-8 paths; timeline watcher and BRAW metadata defer cleanly behind their existing soft-fails); **(3)** toolchain + installer + signing, all now researched to specific picks (CMake + VS2022 + CUDA 12.9; Inno Setup; ship unsigned first — Microsoft's current docs kill the EV-SmartScreen shortcut, making Azure Artifact Signing at $9.99/mo the eventual upgrade). Two findings materially improve the case: **free Resolve loads third-party OFX plugins** (BMD staff, on record — the audience is every Windows Resolve user), and NDI's license not only permits but **recommends bundling the runtime DLL**. The one non-negotiable: a **Windows machine with an NVIDIA GPU** — CUDA cannot be compiled from macOS and Tiers 1–2 cannot run in CI, so the hardware is both a Phase-0 prerequisite and a standing per-release cost. Realistic effort: **~2.5–4.5 focused weeks** to a released Windows installer with the CUDA path (§9), plus an optional parity tail. Decision to make first: commit to the CUDA-native pipeline or don't start — a CPU-only Windows build would demo well and then fail the 8K immersive workflows this plugin exists for.

Labels: **(P)** verified primary source · **(U)** user-reported/secondary · **(T)** needs testing · **(L)** verified locally on this machine.

---

## 1. What exists today — the honest inventory

### 1.1 The May-2025 scaffold, and what it actually is

Commit `50eacc1` ("Trying to build on windows and failing", 2025-05-28) added a Windows/CUDA scaffold that has not been touched since. The plugin has since gained its entire modern architecture — the process-shared sender hub and eye pairer (#6, v1.5), the GPU-native fused downscale+convert path and async pump (#5, v1.6–1.6.1), the STMap warp (#7, v1.7–1.9), camera-metadata projection + the timeline watcher (#11, v1.10–1.11), and the stereo canvas guard (#12) — none of which the scaffold knows about. (L)

| Artifact | State |
|---|---|
| [CMakeLists.txt](../CMakeLists.txt) | Cross-platform harness, **stale**: compiles only `NDIOutputPlugin.cpp` + the platform GPU file. Missing vs the Makefile: `BRAWImmersiveReader.cpp`, `TimelineClipWatcher.cpp`, `MacFileDialog.mm` (macOS), zlib (`-lz`, required by `STMap.h` on every platform), `-Ithird_party/braw`. Its `target_compile_definitions(... kPluginVersionString=...)` collides with the `#define kPluginVersionString` now in the source. On Windows it links no `d3d11.lib` while the source calls `D3D11CreateDevice` (one immediate link failure), installs a **flat** `NDIOutput.ofx` + NDI DLL into the Plugins root rather than the `.ofx.bundle` tree the OFX spec requires (§6.1), and hardcodes CUDA architectures `52;61;75;86` — a list that no longer compiles on current toolkits (§5.1). (L) |
| [src/CudaGPUAcceleration.cu](../src/CudaGPUAcceleration.cu) (460 lines) + [.h](../src/CudaGPUAcceleration.h) | Two kernels — `rgba_to_uyvy_kernel` (Rec.709 SDR) and `rgba_to_hdr_p216_kernel` (Rec.2020/BT.2100 limited-range) — behind a **host-memory** API: upload full-res float RGBA → convert → download → sync. This mirrors the plugin's pre-v1.5 design generation. It has none of: fused box-downscale, STMap warp variants, the non-blocking slot-ring submit + completion callbacks, device-buffer passthrough copy/readback, or per-queue map upload — i.e. none of the ~20-function contract in [src/MetalGPUAcceleration.h](../src/MetalGPUAcceleration.h) that `renderMetalFrame` actually drives. It also calls `cudaDeviceReset()` in shutdown — inside Resolve, which runs its **own** CUDA work in the same process, that destroys the host's primary-context state and must never ship. (L) |
| [scripts/build_windows.bat](../scripts/build_windows.bat) | MSVC 2022 + Ninja + CMake driver; salvageable once CMakeLists is fixed. Installs to both `C:\Program Files\Blackmagic Design\DaVinci Resolve\OFX\Plugins` and `C:\Program Files\Common Files\OFX\Plugins` — only the latter is the spec path (§6.1). (L) |
| [scripts/build_windows_mingw.bat](../scripts/build_windows_mingw.bat) | **Dead end — delete.** nvcc on Windows only accepts MSVC `cl.exe` as host compiler; a MinGW/gcc host build of the CUDA TU can never work (§5.1). (P) |
| [README_WINDOWS_CUDA.md](../README_WINDOWS_CUDA.md) | **Aspirational, not descriptive.** It documents automatic CPU fallback logic, memory pooling, multi-stream processing, "Graceful Fallback / Device Reset" recovery, and a benchmark table (RTX 3080 timings, 8x speedups) for code that does not do these things and has never run. Treat as fiction; replace it with real docs when the port lands. (L) |
| BUILD.md / LEARNINGS.md | BUILD.md says "does not build yet"; LEARNINGS has the open item: *"Record the actual failure output here when work resumes — 'failing' without the error text is unactionable."* No failure text was ever recorded. (L) |
| CI / installer | No `.github/` directory, no CI of any kind, no vcxproj, no Windows installer tooling. (L) |

### 1.2 Platform-dependency inventory of the shipping code

Everything below is from a full read of [src/NDIOutputPlugin.cpp](../src/NDIOutputPlugin.cpp) (3,620 lines) and the modules it pulls in. The good news: the platform split is already disciplined — `#ifdef __APPLE__` / `#ifdef _WIN32` throughout, and the whole domain layer is platform-neutral C++.

**Already portable as-is (P/L — plain C++17, no platform APIs):**
`StereoPair.h` (eye pairer, SbS/TB packers, canvas guard), `STMap.h` (EXR reader — needs zlib on Windows), `BRAWLensMap.h` (calibration JSON → equirect maps; deliberately SDK-free), `StreamResolution.h`, `RenderProbe.h`, the SenderHub (NDI sender sharing + status), all `hubSubmitFrame` pairing/packing/degrade logic, and the whole OFX action plumbing. The unit tests (`make test`) are equally portable (`<zlib.h>`, `<sys/stat.h>` — both fine under MSVC).

**Platform-specific, with the Windows story for each:**

| Subsystem | Today (macOS) | Windows story |
|---|---|---|
| Logging | `os_log` via `NDI_LOG`; non-Apple falls back to `printf` | `printf` goes nowhere useful in a GUI host. Route `NDI_LOG` to `OutputDebugStringA` (viewable live in DebugView/WinDbg) and/or an append-only file; the Tier-2 loop depends on live logs (§7). Small, mechanical. |
| GPU declaration | `describe()` sets `kOfxImageEffectPropMetalRenderSupported` under `__APPLE__` **only** — a Windows build today declares *no* GPU support and would receive CPU buffers | Declare `kOfxImageEffectPropCudaRenderSupported` (+ `CudaStreamSupported`) under `_WIN32`; add a `renderCudaFrame` twin of `renderMetalFrame` (§3). |
| GPU module | `MetalGPUAcceleration.mm` (1,286 lines, 6 MSL kernels, slot-ring async submit, per-queue buffer cache) | Full CUDA rewrite of the module against the same header-level contract (§3). This is the core of the port. |
| Async pump | `AsyncPump` (+`pumpWorkerLoop`, `pumpOnConvertDone`) is `__APPLE__`-gated only because it is fed by Metal completion callbacks | The design ports unchanged: CUDA fires the same `done` callback from `cudaLaunchHostFunc`/`cudaStreamAddCallback` on the host-provided stream. Un-gate it to "any GPU-native platform". |
| Browse buttons | `MacFileDialog.mm` (NSOpenPanel); the push-button params themselves are `__APPLE__`-gated, so a Windows build compiles without them | Either ship without (path text fields still work) or add ~100 lines of `IFileOpenDialog`. Low risk either way. |
| Timeline (Auto) camera clip | `TimelineClipWatcher.cpp` is POSIX to the bone: `posix_spawn`, pipes, `waitpid`, `SIGTERM`, `dladdr` for bundle-relative paths; spawns `ndi_timeline_watch.py`, which appends the **macOS** Resolve scripting Modules path | Port = `CreateProcessW` + anonymous pipes + `GetModuleFileNameW`, plus platform-branching the .py: Windows scripting lives at `%PROGRAMDATA%\Blackmagic Design\DaVinci Resolve\Support\Developer\Scripting` with `fusionscript.dll` at `C:\Program Files\Blackmagic Design\DaVinci Resolve\fusionscript.dll` (P — Resolve's own scripting README documents all three platforms), and "a python3 on PATH" is a much weaker assumption on Windows (T). Feature already fails soft (Manual Path keeps working) — safe to defer. |
| Camera-metadata (BRAW) reads | `BRAWImmersiveReader.cpp`: CoreFoundation strings + the vendored **Mac** dispatch shim; non-Apple already degrades to *"Camera-metadata projection is macOS-only for now"* | Portable when wanted: the BRAW SDK ships a `Win/` variant whose dispatch shim `LoadLibraryExW`s `BlackmagicRawAPI.dll`, and its IDL contains the same immersive attributes (`OpticalProjectionKind`, `OpticalProjectionData`, …) (L — verified in `/Applications/Blackmagic RAW/Blackmagic RAW SDK/Win/Include/`). Windows adds a MIDL step (the API header is generated from `BlackmagicRawAPI.idl` at build time) and needs Resolve's shipped `BlackmagicRawAPI.dll` located at runtime (T). Defer to a late phase; the soft-fail is already in place. |
| Dead scaffolding | — | The `_WIN32` D3D11 "fallback" (creates a device, converts nothing, then logs "using CPU fallback for now") and the vestigial OpenGL includes should be **deleted**, not ported — NDI needs no D3D, and the real fallback is the CPU path. (L) |
| File-path handling | `stat()`/`fopen()` on UTF-8 paths from OFX params | On Windows, narrow-char CRT calls go through the ANSI code page — non-ASCII STMap/BRAW paths would fail. Convert UTF-8 → UTF-16 and use `_wstat`/`_wfopen` at the few touchpoints (`stmapFileStat`, the EXR reader open, BRAW path). Small but easy to forget. (T) |
| zlib | Apple system `-lz` | Not a Windows system library: take `zlib` via vcpkg at build time (or vendor miniz). (P) |

### 1.3 What the render path would do on Windows as scaffolded — and why that decides the architecture

`render()` reads `kOfxImageEffectPropMetalEnabled` only under `__APPLE__`. A Windows build therefore takes the CPU branch: `memcpy` passthrough + `sendCPUFrameToNDI` (CPU box-downscale or CPU STMap warp, then convert+send). That path is complete and platform-neutral — **a CPU-buffers-only Windows port is genuinely functional**, including stereo pairing, projection, and HDR. The scaffold's CUDA kernels would only accelerate the *conversion* of frames that already crossed to CPU memory — and since the downscale runs on the CPU *before* conversion, they buy almost nothing.

The catch is the same arithmetic that shaped v1.6 (issue #5, and [the stereo feasibility study](2026-08-28-resolve-stereo-program-tap-feasibility.md) §4-A2): when the host must hand CPU buffers, Resolve itself reads back the full-res float RGBA frame — an 8160×7200 float32 eye is ~940 MB, times two eyes per frame. HD/UHD mono monitoring will work on CPU buffers; **8K stereo immersive — the reason this plugin exists — will collapse exactly the way it did on macOS before the GPU-native path.** So the port's value hinges on replicating the CUDA-native device-buffer pipeline, not on the two conversion kernels that exist today. (L/P)

---

## 2. The two decisions that shape the port

### 2.1 GPU coverage: CUDA-native, CPU-only, or CUDA+OpenCL?

What Resolve offers OFX plugins, per Blackmagic's own developer kit shipped with Resolve 21 (README updated 2026-05-12) and its `GainPlugin` sample: **CUDA, OpenCL, and Metal render support**, with CPU processing as the universal fallback. The sample's platform mapping is the template for ours: `setSupportsOpenCLRender(true)` unconditionally, `setSupportsCudaRender(true)` + `setSupportsCudaStream(true)` under `#ifndef __APPLE__`, `setSupportsMetalRender(true)` under `#ifdef __APPLE__` — i.e. on Windows BMD's own code declares **CUDA + OpenCL**, never Metal. (P — `/Library/Application Support/Blackmagic Design/DaVinci Resolve/Developer/OpenFX/README.txt` + `GainPlugin/GainPlugin.cpp` lines 389–400, L)

Corroborating primary sources for the Windows side:

- The GPU properties are no longer a Resolve-only extension: **OpenFX 1.5 (2024-08-29) standardized the CUDA/Metal/OpenCL render properties** in [`ofxGPURender.h`](https://github.com/AcademySoftwareFoundation/openfx/blob/main/include/ofxGPURender.h) ("CUDA rendering was added in version 1.5"), originating from BMD's `ofxImageEffectExt.h` — OFX TSC: *"BMD's extension header file ofxImageEffectExt.h defines a set of properties which enable hosts to pass images to plugins as Cuda, Metal or OpenCL buffers … This mechanism has seen widespread adoption"* ([issue #98](https://github.com/AcademySoftwareFoundation/openfx/issues/98#issuecomment-1307555905)). (P)
- Resolve 21's Windows system requirements, per BMD's Peter Chamberlain in the official release post: *"GPU which supports OpenCL 1.2 or CUDA 12.8 … NVIDIA Studio driver 581.57 or newer."* ([forum t=236904](https://forum.blackmagicdesign.com/viewtopic.php?f=21&t=236904)) — i.e. Resolve's Windows compute stack is CUDA on NVIDIA and OpenCL 1.2 on AMD/Intel, nothing else. (P — vendor-staff post)
- What happens to a CUDA-only plugin on an AMD/Intel Windows machine is **not stated verbatim anywhere BMD-official** (unverified as a quote); the spec mechanism, however, is unambiguous: the host only *"MAY set"* `CudaEnabled` when both sides support CUDA — otherwise `kOfxImagePropData` *"is a CPU memory pointer"* and the plugin's CPU path renders. (P — ofxGPURender.h)
- Since OFX **1.5.1** (2025-11-20) a plugin can also declare CPU capability explicitly via `kOfxImageEffectPropCPURenderSupported` ([release notes](https://github.com/AcademySoftwareFoundation/openfx/releases/tag/OFX_Release_1.5.1)) — not needed here (our CPU path is real), noted for completeness.
- **Free Resolve loads third-party OFX plugins — confirmed by BMD staff.** Shrinivas Ramani (Blackmagic Design), relaying Peter Chamberlain's canonical answer: free Resolve is *"fully capable"* and *"Optional installers - like the free Fairlight sound library or third party effects (OpenFX /VST/other) - work with both."* ([forum t=204746](https://forum.blackmagicdesign.com/viewtopic.php?f=18&t=204746)) The current product-page comparison has no OpenFX row either way; the lingering "Studio required" folklore traces to Resolve **Lite** 11-era threads. So the Windows audience is *all* Resolve users, not just Studio. (P — vendor-staff forum)

One porting footnote from the same header: the vendored `openfx/include/` in this repo predates the standardization — it has the ext-header property set but **not** `kOfxImageEffectPropCudaStreamSupported`/`kOfxImageEffectPropCudaStream`. Take the CUDA constants from Resolve's shipped `OpenFX-1.4/include/ofxGPURender.h` (or vendor the upstream 1.5 header) as part of the port. (L)

The OpenFX contract makes the fallback story explicit: a plugin that declares GPU support **must** remain capable of CPU rendering — the property doc reads "'true' … in the case of plug-ins this also means that it is capable of CPU based rendering in the absence of a GPU" (P — vendored [openfx/include/ofxImageEffectExt.h](../openfx/include/ofxImageEffectExt.h) lines 23–33). The host "MAY" then enable CUDA per render call via `kOfxImageEffectPropCudaEnabled`; when it doesn't (non-NVIDIA machine, or host choice), the same render action receives CPU pointers and our existing CPU path runs. That is precisely how the Metal path is structured today (`MetalEnabled` checked per render), so the shape is already proven in this codebase.

| Option | Work | Who it serves | Verdict |
|---|---|---|---|
| **A. CPU buffers only** (declare nothing) | Small (§4 plumbing only) | Every Windows GPU vendor, at HD/UHD monitoring rates; 8K stereo unusable | Fine as *Phase 1* to get a testable plugin, not as the destination |
| **B. CUDA-native fused pipeline** (recommended) | The real port (§3) | NVIDIA machines at full quality/resolution — the realistic Resolve-on-Windows immersive workstation; AMD/Intel machines automatically inherit Option A behavior via the spec's CPU fallback | **Build this** |
| **C. B + OpenCL path for AMD/Intel** | Duplicate all 6 kernels + a third submit/completion machinery in OpenCL's C API; buildable with only Khronos headers + ICD loader (§5.4) | AMD/Intel Windows users at full resolution | Defer until someone actually asks; every future kernel change would then cost ×3 |

Recommendation: **B**, explicitly documenting that non-NVIDIA Windows machines run the CPU path with a resolution ceiling. Option C stays on the shelf — the OpenCL properties are already in the vendored headers if demand materializes.

### 2.2 NDI runtime on Windows: bundle the DLL (like macOS) or require the NDI runtime installer?

The macOS release bundles `libndi_advanced.dylib` inside the bundle with an `@loader_path` reference. The NDI Advanced SDK 6.2 manual (L — extracted from the SDK's own PDF on this machine) answers the Windows equivalent directly, and more strongly than expected:

- **Bundling is the vendor-recommended mechanism**: *"You should include the NDI DLLs as part of your own application and keep them in your application folders so that there is no chance that NDI DLLs installed by your application might conflict with other applications on the system that also use NDI. Please do not install your NDI DLLs into the system path for this reason."* (P — NDI Advanced SDK manual §3 "Licensing") Distribution of `Bin\*.*` is licensed *"if your EULA terms cover the specific requirements of the NDI SDK EULA, and your application covers the terms of the License section"* (§5.2).
- **The runtime DLL name is `Processing.NDI.Lib.x64.dll`** — the SDK's own header (Advanced SDK included) defines `NDILIB_LIBRARY_NAME "Processing.NDI.Lib.x64.dll"` for Win x64 (P — `Processing.NDI.Lib.h` lines 51–53, L; same text at [docs.ndi.video dynamic-loading](https://docs.ndi.video/all/developing-with-ndi/sdk/dynamic-loading-of-ndi-libraries.md)). The `libndi_advanced.dylib` name is an Apple-SDK link-library quirk. Whether the Windows **Advanced** SDK additionally ships an Advanced-named DLL, and its import-lib name, could **not** be verified from primary web sources — the repo's CMake scaffold records `Lib/x64/Processing.NDI.Lib.x64.lib` and `Bin/x64/Processing.NDI.Lib.x64.dll` under `C:\Program Files\NDI\NDI 6 Advanced SDK`, presumably transcribed from the actual May-2025 install (U) — confirm on the Windows machine in Phase 0.
- **The runtime-installer alternative exists** but is the weaker option here: apps may instead ship nothing and point users at the NDI runtime download (`NDILIB_REDIST_URL` = `http://ndi.link/NDIRedistV6`, which live-resolves today to `https://downloads.ndi.tv/SDK/NDI_SDK/NDI 6 Runtime.exe` (P)), locating it at run time via the `NDI_RUNTIME_DIR_V6` environment variable (`NDILIB_REDIST_FOLDER`). An installer may also embed and silently run the official redistributable (`/verysilent`), with an obligation to *"make all reasonable effort to keep the versions you distribute up to date"*. (P — manual §5.3, §7.1; [software-distribution](https://docs.ndi.video/all/developing-with-ndi/sdk/software-distribution.md))
- **Attribution obligations** (identical to what macOS already ships): a link to `https://ndi.video/` near where NDI is used, in docs, and on the site; the *"NDI® is a registered trademark of Vizrt NDI AB"* line; and the third-party notices file — on Windows named **`Processing.NDI.Lib.Licenses.txt`** — *"included beside the NDI binary files."* (P — manual §3, §22 "3rd party rights")
- **The Advanced SDK remains required on Windows** — not for API reasons but for HDR: *"Systems licensed to integrate NDI Advanced SDK … Can send HDR content"* while plain-SDK integrations *"Cannot send content with HDR metadata."* Same section adds: *"When using the trial version of the NDI Advanced SDK, sending HDR content is limited to 30 minutes"*, and the docs licensing page echoes *"The standard 30-minute trial timeout applies to supported non-mobile platforms."* What formally separates "trial" from licensed Advanced use is not defined in the primary sources — this is a pre-existing, platform-neutral open question for the free plugin (the shipped macOS releases already live under whatever this status is; v1.13 shipped without a vendor ID per the rule that a vendor ID is needed *"to use the Advanced SDK in a commercial product or environment"* — [advanced-sdk licensing](https://docs.ndi.video/all/developing-with-ndi/advanced-sdk/licensing.md)). Worth an email to licensing@ndi.video before the Windows release. (P — manual §19.1.1, §3; [docs licensing](https://docs.ndi.video/all/developing-with-ndi/sdk/licensing.md); see §8)
- **Version currency**: the current SDK is **6.3.2** (P — docs.ndi.video references throughout); the Mac machine's installed Advanced SDK is 6.2.0 (L). The EULA requires releases to build against an SDK *"less than thirty (30) days old if there is one"* (P — NDI Advanced License Agreement §2.b). The same rule already governs macOS releases; the Windows pipeline inherits it — and the Advanced SDK download is access-gated (*"part of NDI Advanced, our commercial license"* request flow, [advanced-sdk](https://docs.ndi.video/all/developing-with-ndi/advanced-sdk.md)), so getting the Windows Advanced SDK download is a Phase-0 step, not a build-day step.

**Recommendation: bundle the DLL inside the bundle** — matching macOS, the vendor's stated preference, and zero-dependency installs. The one real technical caveat: Windows does **not** search the plugin's own directory for a load-time DLL import, so "just put it next to the .ofx" is not enough by itself — §6.2 has the mechanics and the fix (delay-load or explicit `LoadLibrary`).

---

## 3. The CUDA port, precisely scoped

The contract to replicate is [src/MetalGPUAcceleration.h](../src/MetalGPUAcceleration.h) — ~20 C functions the plugin drives, of which the ones that matter are:

1. **Fused kernels (the heart)**: `downscale_rgba_to_uyvy`, `downscale_rgba_to_p216`, `warp_rgba_to_uyvy`, `warp_rgba_to_p216` — box-downscale (divisor 1/2/4) + vertical flip + color conversion in one pass over the host's device-resident float RGBA buffer, plus the two legacy full-res converters. Six kernels total; the CUDA file has two, and even those differ from the Metal versions (no downscale, no warp). Porting MSL→CUDA is mechanical — same math, and `make test`'s CPU references (`ndi_stream::downscaleRGBABox`, `ndi_stmap::warpRGBABox`, the converters) are the ready-made oracles: the identity `CUDA kernel ≡ CPU reference` is exactly what `make test-metal` asserts for Metal today, so a `test_cuda_downscale` twin belongs in the port.
2. **The non-blocking submit ring**: `*_submit(...)` encodes the kernel, returns immediately (`OK`/`BUSY`/`INVALID`), and fires `done(user, slot, outPtr, outBytes, gpuMs, ok)` from the GPU completion; slots are context-owned staging buffers released by the consumer. CUDA mapping: staging = pinned host memory (`cudaHostAlloc`) written by the kernel via mapped/zero-copy or a small `cudaMemcpyAsync` after the kernel on the same stream; completion = `cudaLaunchHostFunc` (or `cudaStreamAddCallback`) on the **host-provided stream**; `gpuMs` = `cudaEvent` pair around the kernel. The AsyncPump consumes identically to today.
3. **Passthrough + readback**: `copy_buffer` (device→device src→dst for the effect output — `cudaMemcpyAsync` D2D on the host stream, no sync) and `read_buffer` (full-frame D2H fallback).
4. **STMap upload cache**: `create_shared_buffer_for_queue` + `queue_device` become `cudaMalloc` + upload once per device, cached on the `StmapEntry` exactly as the Metal buffers are (the `metalBufferByDevice` map generalizes to `void* deviceKey`).

**The host-side contract** (P — Resolve-shipped `OpenFX-1.4/include/ofxGPURender.h`, and the same text in the upstream OpenFX spec): with `kOfxImageEffectPropCudaEnabled=1` the image `kOfxImagePropData` pointers are CUDA device memory; declaring `kOfxImageEffectPropCudaStreamSupported="true"` gets `kOfxImageEffectPropCudaStream` (a `cudaStream_t`) per render, and then the plugin *"SHOULD ensure that its render action enqueues any asynchronous Cuda operations onto the supplied queue"*, *"SHOULD NOT wait for final asynchronous operations to complete before returning"*, and *"SHOULD NOT call cudaDeviceSynchronize() at any time."* That is a word-for-word match for the architecture the plugin already has on Metal (encode, return, complete off-thread) — the port is a translation, not a redesign. Resolve's README adds the inverse rule: without stream support the plugin must synchronize before returning. (P/L)

**`NDIOutputPlugin.cpp` changes**: `describe()` declares the CUDA properties under `_WIN32`; `render()` grows the `CudaEnabled` branch calling `renderCudaFrame` (a structural twin of `renderMetalFrame`: passthrough copy → `ensureNDIReady` → fused submit via pump → readback fallback); `initializeGPUContext`/`shutdownGPUContext` swap the CUDA context in and drop the D3D11 corpse; the existing `#elif defined(_WIN32)` conversion branches collapse into the same shape the Apple side has. The `AsyncPump` un-gates. Estimated new/ported code: ~800–1,200 lines of CUDA/C++ replacing the 460-line sketch, plus a `tests/test_cuda_downscale.cu`.

---

## 4. Windows plumbing beyond the GPU (from §1.2)

Phase-ordered, with the soft-fail features last — everything here is bounded, known work:

1. **Logging** — `NDI_LOG`/`NDI_LOG_TEXT` → `OutputDebugStringA` + optional file sink; add a `scripts/monitor_ndi_logs.ps1` note (DebugView/`Get-WinEvent` equivalent) so the Tier-2 loop survives the platform move.
2. **zlib** — vcpkg `zlib` (or vendor miniz and drop the dependency everywhere).
3. **UTF-8 paths** — `_wstat`/`_wfopen` shims at `stmapFileStat`, the EXR reader, and (later) the BRAW path.
4. **Browse buttons** — `IFileOpenDialog` twin of `MacFileDialog` (~100 lines), or ship Phase 1 without (params are already gated).
5. **Timeline (Auto) watcher** — `CreateProcessW`+pipes port of `TimelineClipWatcher`, .py gains the Windows Modules path (`%PROGRAMDATA%\Blackmagic Design\DaVinci Resolve\Support\Developer\Scripting\Modules`, `fusionscript.dll` — P, Resolve scripting README) and a python3 discovery story (`py -3`? bundled?) (T). Defers cleanly — Manual Path mode works without it.
6. **Camera-metadata (BRAW)** — MIDL-generate the API header from the Win SDK's IDL, port the reader to the Windows dispatch shim (`BlackmagicRawAPI.dll`), verify Resolve-shipped DLL resolution at runtime (T). Defers cleanly — soft-fails to passthrough today.

---

## 5. Build toolchain & CI

### 5.1 The hard constraints (all P, docs.nvidia.com)

- **nvcc on Windows requires MSVC (`cl.exe`) as host compiler.** The CUDA Windows install guide's compiler-support table lists only MSVC (VS2019 16.x, VS2022 17.x, VS2026 18.x for CUDA 13.3); MinGW/gcc appears nowhere. ([CUDA Installation Guide for Windows](https://docs.nvidia.com/cuda/cuda-installation-guide-microsoft-windows/index.html)) → `build_windows_mingw.bat` can never work; delete it.
- **No cross-compilation from macOS (or Linux) to Windows.** NVIDIA documents no Windows-target cross-toolchain, and macOS CUDA ended entirely at 10.2: *"CUDA 10.2 … is the last release to support macOS."* ([10.2 release notes](https://docs.nvidia.com/cuda/archive/10.2/cuda-toolkit-release-notes/index.html)) → every CUDA compile happens on a Windows machine or Windows CI runner. This is the single biggest workflow change for a Mac-based maintainer.
- **The architecture list in CMakeLists is now version-locked.** CUDA 13.0 removed offline compilation for Maxwell/Pascal/Volta: *"newer toolkits will be unable to target these architectures … CUDA 13.0 supports all NVIDIA architectures from Turing through Grace Blackwell."* ([13.0 release notes](https://docs.nvidia.com/cuda/archive/13.0.0/cuda-toolkit-release-notes/index.html)) So the current `52;61;75;86` cannot build under CUDA 13.x (`sm_52`/`sm_61` are gone; minimum is `sm_75`/Turing). The pincer: CUDA 12.x keeps Pascal (GTX 10-series) but supports at most VS2022 (the 12.9.1 guide lists only VS2019/VS2022 — [archive](https://docs.nvidia.com/cuda/archive/12.9.1/cuda-installation-guide-microsoft-windows/index.html)); CUDA 13.3 adds VS2026 but starts at Turing.
- **Current CUDA Toolkit as of this writing: 13.3** ([docs.nvidia.com/cuda](https://docs.nvidia.com/cuda/)); supported OS: Windows 10 22H2, Windows 11 (23H2–25H2), Server 2022/2025.

**Recommendation:** pin **CUDA 12.9 + VS2022** for the first release — it keeps `sm_61` (GTX 10-series owners can at least run the plugin, even if 8K needs more card), matches the `windows-2022` CI image, and sits right on Resolve 21's own Windows floor (*"OpenCL 1.2 or CUDA 12.8 … NVIDIA Studio driver 581.57 or newer"* — BMD, §2.1), so any driver that runs Resolve 21 runs our CUDA 12.x binary. Compile `61;75;86` plus PTX from the newest real target for forward-compatibility on Ada/Blackwell. Revisit (drop to `75+`, move to CUDA 13.x) when 10-series support stops being worth the older toolchain.

### 5.2 CMake as the harness

First-class CUDA language support is the current best practice and is what the scaffold already reaches for: `project(... LANGUAGES CXX CUDA)`, [`CMAKE_CUDA_ARCHITECTURES`](https://cmake.org/cmake/help/latest/variable/CMAKE_CUDA_ARCHITECTURES.html) (CMake ≥3.18), and [`find_package(CUDAToolkit)`](https://cmake.org/cmake/help/latest/module/FindCUDAToolkit.html) with `CUDA::cudart` imported targets (≥3.17). (P) The work is repairing the CMakeLists (§1.1 gap list): full source set, vcpkg toolchain for zlib, the version-define collision, correct bundle-layout install, and making the same file able to drive the macOS build so the two platforms can't drift (or, minimally, keeping the Makefile for macOS and CMake for Windows with BUILD.md as the contract — either is defensible; drift risk says one harness).

### 5.3 GitHub Actions CI — compile-only, and worth it

- Windows runner images carry MSVC but **no CUDA**: `windows-2022` → VS2022 17.14; `windows-2025`/`windows-latest` → now a VS2026 image. (P — [actions/runner-images](https://github.com/actions/runner-images) README + per-image software lists; CUDA absent from both.) With the CUDA-12.9 recommendation above, **pin `windows-2022`**, not `windows-latest`.
- Install CUDA per-job either with [Jimver/cuda-toolkit](https://github.com/Jimver/cuda-toolkit) (v0.2.36 current; Windows supported; `method: network` + `sub-packages: '["nvcc","cudart","visual_studio_integration"]'` keeps the download small) or with NVIDIA's own documented silent installer (network installer + `-s` with subpackage names like `nvcc_13.3 cudart_13.3` — [install guide §3.3](https://docs.nvidia.com/cuda/cuda-installation-guide-microsoft-windows/index.html)). (P)
- What CI can and cannot prove: it **can** compile the full plugin + CUDA kernels, run the portable `make test` suite (all host-independent — §1.2), build the installer (§6.3), and attach artifacts to a release. It **cannot** execute CUDA kernels (no GPU on hosted runners), load the plugin into Resolve, or verify an NDI stream — Tiers 1–2 stay human, on real hardware (§7). Compile-only CI is still the difference between "the Windows build rots between releases" and not; this repo has already lived the former.

### 5.4 The OpenCL fallback, if ever wanted (deferred per §2.1)

Buildable with MSVC alone — no vendor SDK: Khronos [OpenCL-Headers](https://github.com/KhronosGroup/OpenCL-Headers) + [OpenCL-ICD-Loader](https://github.com/KhronosGroup/OpenCL-ICD-Loader) (both Apache-2.0), packaged as the vcpkg `opencl` port; you link `OpenCL.lib` and the user's driver-registered ICD is dispatched at run time (the loader "forward[s] API calls to the correct implementation", with vendors registered under `HKLM\SOFTWARE\Khronos\OpenCL\Vendors` — [cl_khr_icd, OpenCL 3.0 ext spec](https://registry.khronos.org/OpenCL/specs/3.0-unified/html/OpenCL_Ext.html)). Kernels compile at run time via [`clBuildProgram`](https://registry.khronos.org/OpenCL/sdk/3.0/docs/man/html/clBuildProgram.html), so no device compiler enters the build. (P) The cost is not the toolchain — it's maintaining a third copy of every kernel and a third submit/completion machinery forever.

---

## 6. Packaging, installer & signing

### 6.1 OFX bundle layout and install location on Windows

From the OpenFX spec's [packaging chapter](https://openfx.readthedocs.io/en/main/Reference/ofxPackaging.html) (P):

- Same bundle shape as macOS: `NDIOutput.ofx.bundle/Contents/<ARCH>/NDIOutput.ofx` (+ optional `Info.plist`, `Resources/`). *"All the binaries must end with '.ofx', regardless of the host operating system"* — on Windows the `.ofx` is a renamed DLL.
- Recognized Windows architecture directories: **`Win32`**, **`Win64`** (our target), and `Win-arm64ec` (ARM64EC; `Win-arm64`/`Win-arm64x` are reserved, "not yet normative"). *"Not all the above architectures need be supported, only the architectures supported by the host product itself."*
- Install search order on Windows: 1) the `;`-separated `OFX_PLUGIN_PATH` env var; 2) `SHGetFolderPath(CSIDL_PROGRAM_FILES_COMMON) + "\OFX\Plugins"` — i.e. `%COMMONPROGRAMFILES%\OFX\Plugins`; 3) the literal `C:\Program Files\Common Files\OFX\Plugins`, which the spec marks *"deprecated"* but which *"should still be examined by hosts"* — and which is what 2) resolves to on English systems anyway, and what BMD's own README and support staff name. Directories are scanned recursively; anything starting with `@` is ignored.
- The spec is **silent on auxiliary DLLs** — the only text is *"Plug-ins are free to include other resources in the Resources subdirectory."* The OFX TSC discussed it ([issue #165](https://github.com/AcademySoftwareFoundation/openfx/issues/165)) and closed without standardizing; TSC chair: *"since OpenFX plugins always use a bundle structure, you can put dependent DLLs in the Resource/ folder"*, and plugin vendor RE:Vision describes the working practice: *"On Windows we can get the ofx load path and set the DLL directory at run time and it will just work … also delayed load"*. So DLL placement is convention + loader mechanics — which is exactly §6.2. (P)

So the installer writes `C:\Program Files\Common Files\OFX\Plugins\NDIOutput.ofx.bundle\Contents\Win64\NDIOutput.ofx` (fixing the current CMake install, which drops a bare `.ofx` in the Plugins root — §1.1).

Blackmagic's shipped README independently confirms the install path — *"On the Windows platform, the directory named 'GainPlugin.ofx.bundle' … needs to be copied to 'C:\Program Files\Common Files\OFX\Plugins'"* — and adds a fact worth recording: Resolve now runs as an **ARM64EC host on Windows ARM64**, scanning architecture directories in the order `Win-arm64x`, `Win-arm64ec`, `Win64`. A CUDA plugin has nothing to offer that platform (no NVIDIA), but the x64 bundle remains loadable under emulation via the `Win64` fallback — out of scope, noted for the future. (P — Resolve OpenFX README, 2026-05-12, L)

### 6.2 The NDI DLL: the one loader landmine, and its three documented fixes

On macOS this problem is solved with `install_name_tool` + `@loader_path/../Frameworks/`. Windows has no equivalent stamp — and the default loader behavior is actively against us (all P, [Microsoft Learn: Dynamic-link library search order](https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-search-order), fetched directly and independently confirmed):

- *"If a DLL has dependencies, then the system searches for the dependent DLLs as if they were loaded by using only their module names. That's true even if the first DLL was loaded by specifying a full path."*
- The standard (SafeDllSearchMode) order searches *"the folder from which the application loaded"* — **the EXE's folder (Resolve's), not the loading DLL's folder** — then system dirs, current dir, `PATH`. The `.ofx`'s own directory appears nowhere in the default list.
- Only if the host itself loads the plugin with `LoadLibraryEx(..., LOAD_WITH_ALTERED_SEARCH_PATH)` does the search instead begin *"in the folder of the executable module that LoadLibraryEx is loading"*, propagating to dependencies (*"its behavior continues until all associated executable modules have been located"*). **Whether Resolve uses that flag is unverified** — nothing documents it, and betting the plugin's load on it would make failures look like "plugin silently missing from the Effects Library" (a load-time import failure means the DLL never loads and describe never runs).

Consequences: the naive port — link `Processing.NDI.Lib.x64.lib` as a normal import with the DLL beside the `.ofx` — may simply fail to load on machines without an NDI runtime on `PATH`. Three Microsoft-documented fixes, in order of preference for this codebase:

1. **Delay-load the NDI import** (`/DELAYLOAD:Processing.NDI.Lib.x64.dll` at link) and, before the first NDI call, extend the search path with the bundle's DLL directory — `GetModuleFileNameW` on the plugin's own module → derive `...\Contents\Win64\` → `SetDllDirectoryW`/`AddDllDirectory` ([LoadLibraryEx docs](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-loadlibraryexa): *"If possible, it is best to use AddDllDirectory"*). Zero changes to the ~20 existing `NDIlib_*` call sites. This is also RE:Vision's shipping practice per the OFX TSC thread (§6.1).
2. **Explicit dynamic load**: `LoadLibraryW` with the absolute DLL path (*"If the string specifies a fully qualified path, the function searches only that path"*), then the NDI SDK's own dynamic-loading contract (`NDIlib_v5_load()` function table, `Processing.NDI.DynamicLoad.h`) — the SDK documents this exact pattern for locating the runtime (§2.2). More invasive (every call site goes through the table) but also enables the graceful "NDI runtime missing → point at NDILIB_REDIST_URL" UX the SDK envisions.
3. Rely on the host passing `LOAD_WITH_ALTERED_SEARCH_PATH` — **do not**; unverified (above).

**Recommendation:** fix 1 (delay-load + `AddDllDirectory`), DLL at `Contents\Win64\Processing.NDI.Lib.x64.dll` beside the `.ofx`, `Processing.NDI.Lib.Licenses.txt` in `Contents\Resources\`. Verify on the Phase-1 test machine with no NDI runtime installed and nothing NDI on `PATH` — that is the end-user machine this must work on.

### 6.3 Installer technology — Inno Setup, and why

The payload is one folder tree plus a licenses file, installed to `{Common Files}\OFX\Plugins` with elevation, uninstall, and silent-install support. All three candidates can do it (all P, official docs fetched 2026-08-30):

| | [Inno Setup](https://jrsoftware.org/isinfo.php) | [NSIS](https://nsis.sourceforge.io/Main_Page) | [WiX Toolset](https://www.firegiant.com/wixtoolset/) |
|---|---|---|---|
| Current | 7.1.0 (2026-08-12) | 3.12 (2026-04-19) | v7.0.0 (2026-04); v3–v5 "out of community support" |
| License | Free incl. commercial ([license.txt](https://jrsoftware.org/files/is/license.txt)); since 7.x the site *requests* (not requires) a paid license from commercial users — *"It is not strictly required"* ([order page](https://jrsoftware.org/isorder.php)) | zlib, unconditional ([license](https://nsis.sourceforge.io/License)) | MS-RL source; v6+ binaries carry the Open Source Maintenance Fee EULA — fee triggered only *"if you use this project to generate revenue"* ([README](https://github.com/wixtoolset/wix)) |
| Target path | `{commoncf64}\OFX\Plugins\...` ([constants](https://jrsoftware.org/ishelp/topic_consts.htm)) | `$COMMONFILES64\OFX\Plugins\...` ([Ch. 4](https://nsis.sourceforge.io/Docs/Chapter4.html)) | `CommonFiles64Folder` |
| Folder tree | One `[Files]` line: `recursesubdirs createallsubdirs` ([files section](https://jrsoftware.org/ishelp/topic_filessection.htm)) | scripted | `<Files Include="...\**"/>` since v5 ([release post](https://www.firegiant.com/blog/2024/4/5/wix-v500-has-been-released/)) |
| Elevation | `PrivilegesRequired=admin` is the **default** ([doc](https://jrsoftware.org/ishelp/topic_setup_privilegesrequired.htm)) | `RequestExecutionLevel admin` (default admin) | MSI `ALLUSERS=1` per-machine |
| Uninstall | Automatic (`Uninstallable=yes` default, [doc](https://jrsoftware.org/ishelp/topic_setup_uninstallable.htm)) | **Hand-scripted** `Section "Uninstall"` + ARP registry keys | Automatic (MSI) |
| Silent | `/SILENT`, `/VERYSILENT` ([cmdline](https://jrsoftware.org/ishelp/topic_setupcmdline.htm)) | `/S` | `msiexec /qn` ([msiexec](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/msiexec)) |
| Signing hook | `SignTool` directive built in ([doc](https://jrsoftware.org/ishelp/topic_setup_signtool.htm)) | manual | signtool on the .msi |
| CI (GitHub runners) | **Preinstalled** (6.7.1) on both `windows-2022` and `windows-2025` images (P — [runner-images](https://github.com/actions/runner-images) readmes); CLI `iscc script.iss` | Preinstalled on `windows-2022` only (3.10); absent from `windows-2025` | Only the unsupported v3.14 preinstalled; modern WiX = per-job .NET tool install |

**Recommendation: Inno Setup.** Smallest script (~30 lines), uninstaller and Add/Remove entry for free, admin default, silent flags, a built-in signing hook, and it is already on the CI images — the closest Windows analog to what `package_release.sh` does with `productbuild`. WiX/MSI is the "enterprise deployment" answer (GPO, `msiexec /qn` fleet installs) and can be revisited if studio customers ever ask for MSI; NSIS buys nothing here except hand-written uninstall logic. One footnote for the file: LSVR is a for-profit company, so Inno's *voluntary* commercial-license request nominally applies even though the plugin is free — a goodwill call, not a legal condition (P — order page quote above).

The installer contents mirror the macOS pkg one-to-one: the bundle tree → `{commoncf64}\OFX\Plugins\`, `Processing.NDI.Lib.Licenses.txt` inside `Contents\Resources\` (or beside the DLL, per the NDI manual), a readme carrying the ndi.video link + trademark line (reuse the pkg's `Readme.html` text), the repo LICENSE, and a `SHA256SUMS.txt` entry alongside the existing macOS artifacts in the GitHub release.

### 6.4 Code signing — the landscape shifted; don't buy an EV cert

Findings, all from Microsoft's current documentation (P, fetched 2026-08-30):

- **EV certificates no longer buy SmartScreen reputation.** Microsoft's SmartScreen-for-developers page (updated 2026-05): *"EV certificates no longer bypass SmartScreen. Years ago, signing files with an Extended Validation (EV) code signing certificate would result in positive SmartScreen reputation by default, but this behavior no longer exists… Paying a premium for EV solely to avoid SmartScreen warnings is no longer justified."* Signed-but-unknown files still warn *"until reputation accumulates"* — the difference from unsigned is that the publisher name shows, and reputation attaches to the certificate so it **carries across releases**; unsigned files *"must build reputation anew with every update."* ([smartscreen-reputation](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/smartscreen-reputation))
- **OV certificates got more painful in 2023**: the CA/B Forum baseline requires subscriber private keys in FIPS 140-2 L2+ hardware since 2023-06-01 ([CSC-13](https://cabforum.org/2022/04/06/ballot-csc-13-update-to-subscriber-key-protection-requirements/), [CSC-17](https://cabforum.org/2022/09/27/ballot-csc-17-subscriber-private-key-extension/)) — i.e. a USB token or cloud HSM, awkward in CI.
- **Azure Trusted Signing is now "Artifact Signing"**: fully managed signing, **from $9.99/month** (Basic: 5,000 signatures/mo), needs a *paid* Azure subscription; org validation, or individual validation for devs in the US/Canada. No EV issuance, no reputation shortcut — *"SmartScreen reputation builds up automatically. The prompt stops appearing once the file hash has sufficient download history."* ([overview](https://learn.microsoft.com/en-us/azure/artifact-signing/overview), [quickstart](https://learn.microsoft.com/en-us/azure/artifact-signing/quickstart), [FAQ](https://learn.microsoft.com/en-us/azure/artifact-signing/faq)) A Microsoft Q&A answer still cites a 3-year operating-history rule for org validation (U — not in the current official docs).
- **Unsigned is viable for this audience, with a documented flow**: an internet-downloaded unsigned installer triggers *"Windows protected your PC"* → **More info** → **Run anyway** ([SmartScreen overview](https://learn.microsoft.com/en-us/windows/security/operating-system-security/virus-and-threat-protection/microsoft-defender-smartscreen/), smartscreen-reputation page). Two real costs: enterprise policy can block the continuation entirely, and Windows 11's Smart App Control *"will block execution of unsigned files unless the file has a positive reputation."*
- Mechanics when signing: `signtool sign /fd SHA256 /tr http://timestamp… /td SHA256` on the installer (and the .ofx) ([signtool](https://learn.microsoft.com/en-us/windows/win32/seccrypto/signtool)); Inno's `SignTool` directive signs Setup + uninstaller in the same build.

**Recommendation:** ship v1 **unsigned** with the SmartScreen click-through documented in the README and release notes (this is a niche pro tool distributed from a GitHub project page — the audience Microsoft's own docs describe as needing weeks of "hundreds of clean installs" to build reputation either way), and set up **Azure Artifact Signing** ($9.99/mo, org validation on the LSVR entity) as the upgrade path the moment install-friction reports appear or a studio's policy blocks the unsigned exe. Skip OV tokens and EV entirely — the current Microsoft docs remove EV's one historical advantage.

---

## 7. Testing workflow on Windows

The LEARNINGS §2 loop transplants with these substitutions:

| Loop element | macOS today | Windows equivalent |
|---|---|---|
| Tier 0 compile | `make dev` | `cmake --build` (or `build_windows.bat`); plus the portable unit tests under MSVC and `test_cuda_downscale` on the GPU box |
| Install | `sudo make install` (+ `install_name_tool`) | Copy the bundle to `C:\Program Files\Common Files\OFX\Plugins\` (elevated), or run the Inno installer; no dylib-path fixup exists on Windows — the DLL placement (§6.2) does that job structurally |
| Restart Resolve | required (no OFX hot reload) | identical — same host behavior |
| Plugin cache check | `OFXPluginCacheV2.xml` under `~/Library/Application Support/...` | `%APPDATA%\Blackmagic Design\DaVinci Resolve\Support\OFXPluginCacheV2.xml` — note the extra `\Support\` level. BMD's Dwaine Maggart documents the same delete-to-force-rescan procedure used on macOS: rename the file with Resolve closed, relaunch ([forum t=228353](https://forum.blackmagicdesign.com/viewtopic.php?f=21&t=228353), P — vendor-staff) |
| Live plugin logs | `os_log` → `monitor_ndi_logs.sh` | `OutputDebugStringA` sink (§4) → **DebugView**/WinDbg live capture, plus the optional file sink; port the probe-capture script accordingly |
| Stream verification | NDI Video Monitor.app | **NDI Tools for Windows** (free, v6.3.2 installer at ndi.video/tools) — includes **Studio Monitor** (*"Your window to NDI signals"*), plus Test Patterns as the known-good sender for firewall triage (P — [ndi.video/tools](https://ndi.video/tools/)) |
| HDR validation | SDK example `NDIlib_Recv_HDR` | Same example ships in the Windows SDK's examples; build once on the test box |
| Playback | human presses play (scripting API has no transport control) | identical |
| Host-behavior pitfalls | Render Cache / proxy / stereo-instance table in LEARNINGS §2 | assume identical until re-probed — run the Log Render Calls probe matrix once on Windows (risk #10) |

Two Windows-only additions to the checklist: the **Windows Defender Firewall prompt** on first NDI send (an inbound/outbound rule for the Resolve process — NDI discovery needs mDNS + TCP; document the "Allow" click in the README), and checking the stream from a **second machine** (Windows loopback + firewall interactions can make a local-only check misleading). (U — standard NDI-on-Windows operational lore; verify during Phase 1 Tier 2.)

---

## 8. Risks & open questions

**Hard blockers: none found.** Everything below is work, cost, or an answer to fetch — not a wall.

| # | Item | Type | Notes |
|---|---|---|---|
| 1 | **No Windows/NVIDIA hardware, no port.** CUDA cannot be built from macOS (§5.1), Tiers 1–2 cannot run in CI (§5.3) | prerequisite | A dedicated Windows box with an NVIDIA GPU + Resolve is the Phase-0 purchase/borrow. Every future Windows release needs it again for the Tier-2 pass — this is a *standing* cost, not one-time |
| 2 | ~~Free vs Studio~~ — **resolved**: free Resolve loads third-party OFX (BMD staff on record, §2.1) | resolved | Widens the Windows audience to all Resolve users; test on Studio (what LSVR has) and note free-version reports from users |
| 3 | **NDI Advanced "trial" HDR 30-minute limit wording** (§2.2) | unknown, platform-neutral | The plugin sends HDR without a vendor ID today on macOS. Email licensing@ndi.video for a definitive read before the Windows release; worst case, HDR mode gets a documented caveat while SDR (the default) is unaffected |
| 4 | **Windows Advanced SDK layout unverified** (install path, import-lib name, whether an Advanced-named DLL exists) | unknown | Repo scaffold's paths are probably right (transcribed from a real install, U); confirm in Phase 0. Also: Advanced SDK download is access-gated — request early |
| 5 | **NDI DLL load-time resolution** (§6.2): default Windows search order does not include the `.ofx`'s folder; whether Resolve loads plugins with `LOAD_WITH_ALTERED_SEARCH_PATH` is unverified | risk, mitigated | Delay-load + `AddDllDirectory` (fix 1) makes the question moot; explicitly test on a machine with no NDI runtime installed |
| 6 | **CUDA version pincer** (§5.1): Pascal support ends at CUDA 12.x; VS2026 starts at CUDA 13.3 | decision | Decide the GPU floor once, in Phase 0; document it in the release notes. End users also need drivers new enough for the chosen CUDA runtime — the plugin already fails soft to the CPU path when CUDA init fails, so old-driver machines degrade rather than break |
| 7 | **Timeline (Auto) on Windows**: python3 discovery, `fusionscript.dll` ABI acceptance, `%PROGRAMDATA%` scripting path behavior under a spawned child of Resolve | T | Deferred to Phase 4; Manual Path mode is unaffected. The macOS version already logs-and-degrades when scripting is unavailable — port that behavior verbatim |
| 8 | **BRAW runtime resolution on Windows**: does Resolve's install expose `BlackmagicRawAPI.dll` where the dispatch shim can find it (host-exe-relative)? | T | Phase 4; soft-fails to passthrough by design |
| 9 | **Two GPU backends forever**: every future kernel/feature (new packing, new warp, tone-map changes) is now written twice and tested on two machines | cost | The mitigations are the shared CPU references + per-platform kernel identity tests (§3) and keeping ALL pairing/packing/policy logic in the platform-neutral layer, where it already lives |
| 10 | **Windows-specific Resolve behaviors** (OFX threading cadence, stereo per-eye instance behavior, cache/proxy pitfalls) assumed identical to macOS | T | The probe tooling (Log Render Calls + `RenderProbe`) ports with the plugin — re-run the docs/2026-08-28 probe matrix on Windows before trusting stereo pairing there |
| 11 | **Windows ARM64** (Resolve is ARM64EC there; scans `Win-arm64x`/`Win-arm64ec`/`Win64` — §6.1) | out of scope | No CUDA on that platform; x64 bundle may load under emulation (T, untested). Explicitly unsupported in release notes |
| 12 | **README_WINDOWS_CUDA.md is misleading today** | hygiene | Replace with real docs at Phase 1; until then it misdescribes the product to anyone reading the repo |

---

## 9. Phased plan with effort estimates

Estimates are focused dev-days for someone who knows this codebase, and assume the Phase-0 hardware exists. Each phase ends at a state that is independently shippable or at least testable; the release only happens when Matt calls it (workflow rule).

### Phase 0 — prerequisites (mostly calendar time, ~1 day of hands-on)
- Windows 10/11 x64 machine with an NVIDIA GPU (Turing+ comfortable; the pincer in §5.1 decides whether a Pascal card matters) and DaVinci Resolve installed. This machine is non-negotiable: no cross-compile, and Tiers 1–2 cannot run anywhere else.
- Request/download the Windows **NDI Advanced SDK** (access-gated; §2.2) and confirm its actual install layout + import-lib name (the two NOT-VERIFIED items).
- Install VS2022 + CUDA Toolkit (per §5.1 pincer), CMake, vcpkg.
- Send the licensing@ndi.video email about the Advanced-SDK "trial"/HDR question (risk #3) — its answer takes calendar time, so start it here.

### Phase 1 — it builds and streams, CPU buffers only (~3–5 days)
- Repair CMakeLists (§1.1 gap list), delete the D3D11/OpenGL corpses and the MinGW script, add the logging sink, zlib via vcpkg, UTF-8 path shims.
- No GPU declaration yet — the existing CPU path carries the stream (SDR+HDR, stereo pairing, STMap warp all work; §1.3).
- Produce the spec-correct `NDIOutput.ofx.bundle\Contents\Win64\` layout with the NDI DLL placed per §6.2, install by hand, run the Tier 1–2 loop in Resolve on Windows for the first time. Record every failure in LEARNINGS.md (the standing open item finally gets its error text).
- Exit criteria: NDI Studio Monitor shows the stream from a Windows Resolve at HD/UHD; `make test` equivalents pass under MSVC.

### Phase 2 — the CUDA-native pipeline (~5–10 days)
- Implement the §3 scope: six kernels, submit ring on the host `cudaStream_t`, pump un-gating, `renderCudaFrame`, describe/render wiring, `test_cuda_downscale` against the CPU references.
- Exit criteria: `GPU-native async` log lines on Windows, 8K stereo playback at rates comparable to the Mac path, no CPU-fallback lines during normal playback; kernel outputs byte-identical to CPU references in the test.

### Phase 3 — installer, CI, signing, release (~3–5 days)
- Inno-or-chosen installer per §6.3 (bundle tree → `{commoncf64}\OFX\Plugins`, NDI DLL + `Processing.NDI.Lib.Licenses.txt`, readme with the ndi.video attribution, uninstaller, `/VERYSILENT`).
- GitHub Actions compile-only workflow (§5.3) building plugin + installer artifacts; extend `publish_github_release.sh` to attach the Windows artifacts + SHA256s alongside the macOS pkg.
- Signing decision per §6.4 executed (or explicitly deferred with the SmartScreen flow documented in the README).
- Exit criteria: a fresh Windows machine goes from GitHub release page → installed plugin → visible NDI stream with no manual steps beyond the installer.

### Phase 4 — feature parity tail (optional, ~4–8 days)
- Browse buttons (`IFileOpenDialog`), Timeline (Auto) watcher port, BRAW camera-metadata via MIDL (§4 items 4–6) — in that order of value/effort.
- Until then the Windows release notes state plainly: Camera Metadata projection and Timeline (Auto) are macOS-only; Manual STMap workflows are at parity.

**Total: roughly 2.5–4.5 focused weeks** to a signed(-or-documented) installer with the CUDA path, plus the optional parity tail. The macOS testing-loop discipline (LEARNINGS §2) transplants as-is; budget extra calendar time for the first Windows Tier-2 sessions — every "Resolve behaviors that lie to you" pitfall applies there too.

---

## 10. Key sources

**Local primary sources (L/P — on this machine):**
- Blackmagic Resolve developer kit: `/Library/Application Support/Blackmagic Design/DaVinci Resolve/Developer/OpenFX/README.txt` (updated 2026-05-12 — CUDA/OpenCL/Metal support, Windows install path, ARM64EC scan order, CudaStream rules), `GainPlugin/` sample (GPU declaration pattern; its vcxproj is VS2013/CUDA-8-era — do not copy it, CMake is our harness), `OpenFX-1.4/include/ofxGPURender.h` (CUDA property contract verbatim).
- Resolve scripting: `/Library/Application Support/Blackmagic Design/DaVinci Resolve/Developer/Scripting/README.txt` (Windows scripting paths, `fusionscript.dll`).
- NDI Advanced SDK 6.2.0 (`/Library/NDI Advanced SDK for Apple/`): `documentation/NDI Advanced SDK.pdf` §3 Licensing, §5 Software Distribution, §7 Dynamic Loading, §19.1.1 HDR SDK differences, §23 3rd-party rights; `NDI Advanced License Agreement.pdf` (§2.b 30-day SDK freshness, §2.d distribution grant deferral, §3.d EULA pass-through terms); `include/Processing.NDI.Lib.h` (Windows DLL/redist defines).
- Blackmagic RAW SDK 5.1: `/Applications/Blackmagic RAW/Blackmagic RAW SDK/Win/Include/` (Windows dispatch shim → `BlackmagicRawAPI.dll`; immersive attributes present in `BlackmagicRawAPI.idl`).
- This repo @ `584501c`: full reads of `src/NDIOutputPlugin.cpp`, `src/MetalGPUAcceleration.{h,mm}`, `src/CudaGPUAcceleration.{h,cu}`, `src/TimelineClipWatcher.cpp`, `src/BRAWImmersiveReader.cpp`, `Makefile`, `CMakeLists.txt`, `BUILD.md`, `scripts/*`; `git show 50eacc1`.

**Web primary sources (P — cited inline throughout):**
- OpenFX spec: openfx.readthedocs.io (packaging chapter, GPU rendering) · github.com/AcademySoftwareFoundation/openfx.
- NVIDIA: docs.nvidia.com CUDA Installation Guide for Windows (13.3 + 12.9.1 archive), CUDA 13.0 + 10.2 release notes, nvcc docs.
- CMake: cmake.org FindCUDAToolkit, CMAKE_CUDA_ARCHITECTURES.
- GitHub: actions/runner-images (image ↔ VS matrix), Jimver/cuda-toolkit.
- Khronos: OpenCL-Headers, OpenCL-ICD-Loader, OpenCL 3.0 ext spec (cl_khr_icd), clBuildProgram reference; microsoft/vcpkg `opencl` port.
- NDI: docs.ndi.video (licensing, advanced-sdk licensing, dynamic-loading, software-distribution, example-code) · ndi.video/tools (NDI Tools for Windows incl. Studio Monitor) · live redirect of ndi.link/NDIRedistV6.
- Microsoft Learn: Dynamic-link library search order · LoadLibraryEx (search flags) · SignTool · msiexec/ALLUSERS/UpgradeCode/ARP · Microsoft Defender SmartScreen overview · SmartScreen reputation for app developers (the EV reversal) · Azure Artifact Signing overview/quickstart/FAQ/pricing.
- Installers: jrsoftware.org (Inno Setup 7.1 docs: consts, files, privileges, cmdline, signtool, iscc; license + order pages) · firegiant.com/wixtoolset + github.com/wixtoolset/wix releases/LICENSE (WiX v7, OSMF) · nsis.sourceforge.io (3.12, manual ch. 3–4, license).
- CA/B Forum: ballots CSC-13 and CSC-17 (hardware key requirement for code-signing certs, effective 2023-06-01).
- Blackmagic (vendor-staff forum posts): t=236904 (Resolve 21 Windows GPU/driver requirements — Peter Chamberlain) · t=204746 (free Resolve runs third-party OFX — Shrinivas Ramani relaying Peter Chamberlain) · t=228353 (Windows OFX plugin cache path + rescan procedure — Dwaine Maggart).
- OpenFX GitHub: issue #98 (BMD ext-header standardization), PRs #101/#107, issue #165 (dependent-DLL discussion, TSC chair + RE:Vision practice), OFX 1.5 / 1.5.1 release notes.

**Fact-label reminder:** every inline (P) claim above traces to one of these; (U) items are marked where used (Windows Advanced SDK layout from the repo scaffold; the Azure 3-year org-history rule from a Microsoft Q&A answer; NDI-on-Windows firewall lore). Anything the research could not pin to a primary source says so in place.
