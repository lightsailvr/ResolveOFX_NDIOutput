# Changelog

All notable changes to the NDI Advanced Output Plugin will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.5.0] - 2026-08-28

> Validated 2026-08-28 through the LEARNINGS.md testing loop: unit tests, Tier 0, and Tier 1–2 in Resolve (stereo timeline streams packed stereo; mono unchanged).

### Added
- **Stereo eye pairing** (issue #6): on a native stereo timeline with the Stereo 3D palette at Vision: Stereo, Resolve renders each eye through its own plugin instance; the plugin now pairs the L/R renders for the same frame time — process-globally, across instances — and sends exactly **one packed NDI frame per pair**. Pairing follows the probe findings: keyed on frame time (never arrival order), tolerant of either eye leading by hundreds of ms and of in-eye time reversal, duplicate parked re-renders replace the held frame, and unmated frames age out bounded (`src/StereoPair.h`, unit-tested host-free via `make test`).
- **Stereo Packing** parameter (new Stereo group): Side-by-Side (left eye left) or Top-Bottom (left eye top), matching VR.NDI's projection modes on Quest.
- **Stream Status** parameter: live display of what the stream carries — Mono, Stereo (SbS/TB), a labeled single-eye fallback ("right eye missing — sending left eye only") when the partner eye starves (~1.5 s), or the sender-creation failure. Starvation never freezes or deadlocks the stream; the partner returning restores stereo automatically.
- Filmstrip thumbnail renders are dropped while stereo is active (a 184×92 frame must not hijack the packed stream); mono behavior is unchanged.

### Fixed
- **Stereo mode blacked out the NDI feed, then locked the source name machine-wide** (found in the issue #3 probe session; root-caused in LEARNINGS.md): both per-eye instances created senders under the same name — NDI 6.2 fails a duplicate same-machine name outright — and the failure path's `NDIlib_destroy()` tore the process-wide NDI library out from under the healthy sender, leaking its Bonjour advertisement until Resolve exited. Senders are now **process-shared and refcounted** (one sender per source name, which stereo requires anyway), `NDIlib_destroy()` is never called, and sender-create retries are throttled to every 3 s with the failure surfaced in Stream Status.
- A saved project's NDI Source Name is honored at project load: parameter values are read at createInstance, so the first sender no longer briefly registers under the default name.

## [1.4.0] - 2026-08-28

### Added
- **GPU-native fast path** (issue #5): the plugin now declares Metal render support (`OfxImageEffectPropMetalRenderSupported`), so Resolve hands frames as Metal device buffers instead of full-resolution float32 CPU copies. Fused Metal kernels box-downscale, vertically flip, and color-convert (UYVY, or P216 for HDR) on the GPU **before** any readback — only the small converted frame crosses to the CPU. At 8160×7200/eye that removes a ~940 MB/frame float RGBA transfer.
- **Resolution** control (Basic Settings): stream at **Full**, **Half**, or **Quarter** of the incoming frame. Effective on every path — GPU-native, GPU upload-convert, and pure CPU (box filter, `src/StreamResolution.h`).
- CPU fallback under Metal render: with GPU Acceleration off — or on any kernel failure, or the legacy RGBA output format — the frame is read back whole and the existing CPU path runs, so the stream works in every combination. Log evidence distinguishes the paths (`GPU-native path:` vs `Metal frame full readback -> CPU fallback path`).
- `make test-metal` — GPU kernel correctness tests against the CPU reference on a real Metal device (no Resolve or NDI SDK needed). `make test` additionally runs the new stream-resolution unit tests.

### Fixed
- Latent use-after-free: an async NDI send keeps reading the submitted buffer until the next send call, but send buffers were `resize()`d whenever the frame size changed (routine once the Resolution control lands mid-stream switches). In-flight async sends are now completed (NULL-frame flush) before any send-buffer reallocation.

## [1.3.0] - 2026-08-28

### Added
- Render-call diagnostic probe (issue #3): **Log Render Calls** toggle in a new Diagnostics parameter group, off by default. When on, every render action logs one `NDI Plugin: probe …` line carrying the Resolve page, eye (`OfxImageEffectPropEyeToRender`), frame time, source frame, render-window dimensions, render scale, thumbnail flag (`kOfxImageClipPropThumbnail`), and wall-clock spacing since the previous call. When off, the cost is one parameter read per render.
- Host-property plumbing the stereo work builds on: the instantiating page (`OfxImageEffectPropResolvePage`) is captured at createInstance, and eye/source-frame/thumbnail are read per render.
- `scripts/capture_probe_log.sh` — tees probe lines into `docs/captures/` for the findings report (`docs/2026-08-28-render-call-probe-findings.md`).
- `make test` — host-independent unit tests (probe log-line formatter), no Resolve or NDI SDK required.
- Vendored `openfx/include/ofxImageEffectExt.h` synced with the Resolve 21.0.4 SDK additions (`EyeToRender`, `SrcFrame`, `SrcFilePath`).

## [1.0.3] - 2024-05-27

### Removed
- Legacy build variants (NDIOutputSimple, NDIOutputModern)
- Legacy OFX wrapper class dependencies
- Unused build system complexity
- Old plugin installations to prevent conflicts

### Changed
- Streamlined build system to single modern implementation
- Simplified Makefile with clean targets
- Updated documentation to reflect simplified build process

### Fixed
- Removed potential plugin conflicts from multiple installed versions

## [1.0.2] - 2024-05-27

### Added
- Semantic versioning system with automatic patch increment on builds
- Version increment script (`scripts/increment_version.sh`)
- Version display in plugin description
- `make show-version` command to display current version
- `make dev` command for development builds without version increment
- Comprehensive version management documentation

### Changed
- Build system now automatically increments patch version on `make`
- Plugin description now includes version number
- Updated README with version management section

## [1.0.1] - 2024-05-27

### Added
- Initial version tracking system
- VERSION file for semantic versioning

## [1.0.0] - 2024-05-27

### Added
- Modern OFX C API implementation for maximum compatibility
- Comprehensive HDR support:
  - PQ (ST.2084) and HLG (Hybrid Log-Gamma) transfer functions
  - Rec.2020, DCI-P3, and Rec.709 color spaces
  - Configurable Max Content Light Level (CLL) and Max Frame Average Light Level (FALL)
  - HDR metadata embedding for proper downstream handling
- NDI Advanced SDK v6.1.1 integration
- Real-time streaming with pass-through design
- User-friendly parameter controls
- Professional-grade build system

### Changed
- Complete rewrite from legacy OFX wrapper classes to modern C API
- Improved compatibility with DaVinci Resolve 20+
- Enhanced performance and stability

### Removed
- Dependency on deprecated OFX C++ wrapper classes
- Legacy build system complexity

## [0.x] - Legacy Versions

### Features
- Basic NDI streaming functionality
- Original implementation using OFX wrapper classes
- Limited HDR support 