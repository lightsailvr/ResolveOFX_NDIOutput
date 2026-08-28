# Build & Installation Guide

Canonical instructions for building, installing, and verifying the NDI Output OFX plugin. If a build change lands, this file must be updated in the same PR.

- **macOS** — primary platform, working (Metal GPU path)
- **Windows** — CUDA port in progress, **does not build yet** (see [status](#windows-cuda--status-not-building-yet))

---

## macOS

### Prerequisites

1. **Xcode Command Line Tools**
   ```bash
   xcode-select --install
   ```
2. **NDI Advanced SDK for Apple**, installed at its default location:
   ```
   /Library/NDI Advanced SDK for Apple/
   ```
   Download from [ndi.video/for-developers](https://ndi.video/for-developers/). The Makefile references this path directly — no symlink needed (older docs mentioned `/Library/NDI_Advanced_SDK`; nothing uses it anymore).
3. **DaVinci Resolve** 17+ (developed against Resolve 20/21, Studio).

### Build

```bash
make dev
```

Incremental build; produces `NDIOutput.ofx.bundle/` in the repo root. `make` (default target) is identical — despite older docs, **no target auto-increments the version**. `make clean` removes the bundle, all object files, and the `build/` test-binary directory.

### Unit tests

```bash
make test
```

Host-independent tests in [tests/](tests/) (render-probe log-line formatter, stream-resolution divisor/dims/box-downscale, and the stereo eye-pairer/packers — pairing decisions, reorder window, starvation fallback, SbS/TB byte layout) — compiles in seconds into `build/`, needs neither Resolve nor the NDI SDK. Run it alongside `make dev` before installing.

```bash
make test-metal
```

GPU kernel correctness tests: runs the fused downscale+convert Metal kernels (UYVY and P216) on a synthetic frame — through both the blocking calls and the non-blocking slot-ring submit path (v1.6.0) — and compares against the CPU reference. Needs a Metal device (any Mac; skips cleanly without one), but still no Resolve or NDI SDK. The kernels are compiled from source at runtime, so this is the only pre-Resolve check that catches shader errors — run it whenever `src/MetalGPUAcceleration.mm` changes.

```bash
make bench
```

Pipeline timing harness ([tests/bench_pipeline.mm](tests/bench_pipeline.mm)): reproduces the plugin's per-pair GPU-convert → pack → NDI-send pattern at production 8K dimensions outside Resolve, with an in-process receiver forcing real encode, and reports per-stage timings plus sustained pairs/s. Needs a Metal device **and** the NDI Advanced SDK. Creates NDI source `NDI_BENCH_PIPELINE` (never the production name). Built for the issue #5 performance diagnosis; re-run it when touching the send path or the fused kernels to catch throughput regressions before a Resolve session.

### Install

```bash
sudo make install
```

This does three things (all required):
1. Removes any previous bundle from `/Library/OFX/Plugins/`
2. Copies the new `NDIOutput.ofx.bundle` there
3. Runs `install_name_tool` to rewrite the NDI dylib reference from `@rpath/libndi_advanced.dylib` to its absolute path — without this, Resolve can't resolve the NDI library and the plugin won't load

Then **restart DaVinci Resolve** — OFX plugins are only scanned at startup; there is no hot reload.

### Verify the install

1. After restarting Resolve, check the OFX plugin cache picked up the new binary cleanly (`status="0"` means it described OK):
   ```bash
   grep -B1 'LSVR.NDIOutput' "$HOME/Library/Application Support/Blackmagic Design/DaVinci Resolve/OFXPluginCacheV2.xml" | head -3
   ```
2. In Resolve: Color page → OpenFX panel → **LSVR → NDIOutput**. The plugin's version parameter should show the version you built.
3. For full functional validation, run the testing loop in [LEARNINGS.md](LEARNINGS.md).

### Versioning

The version lives in **two places that must stay in sync**: the `VERSION` file and the `#define kPluginVersion*` macros in [NDIOutputPlugin.cpp](src/NDIOutputPlugin.cpp). Always bump with the scripts, which update both:

```bash
./scripts/increment_version.sh        # bump patch
./scripts/set_version.sh 1.3.0        # set explicit version
./scripts/set_version.sh minor 1.3.0  # or by type
```

Do **not** use `make bump-patch/minor/major` — those only touch the `VERSION` file and let the source drift.

---

## Windows (CUDA) — STATUS: not building yet

The Windows port (CMake + CUDA acceleration) was started in commit `50eacc1` and does not yet produce a working build. Track progress and findings in [LEARNINGS.md](LEARNINGS.md).

What exists so far:
- [CMakeLists.txt](CMakeLists.txt) — cross-platform build (Windows/CUDA, macOS/Metal, Linux stub)
- [scripts/build_windows.bat](scripts/build_windows.bat) (MSVC) and [scripts/build_windows_mingw.bat](scripts/build_windows_mingw.bat) (MinGW)
- [src/CudaGPUAcceleration.cu](src/CudaGPUAcceleration.cu) — CUDA kernels mirroring the Metal path
- [README_WINDOWS_CUDA.md](README_WINDOWS_CUDA.md) — target requirements and intended workflow

Requirements when resuming: Visual Studio 2019/2022 (C++ workload), CUDA Toolkit 11+, NDI 6 Advanced SDK at `C:\Program Files\NDI\NDI 6 Advanced SDK`. Install target is `C:\Program Files\Common Files\OFX\Plugins\`, plus `Processing.NDI.Lib.x64.dll` copied alongside the `.ofx`.

---

## Receiving the stream (test receivers)

- **NDI Video Monitor** (installed at `/Applications/NDI Video Monitor.app`) — quickest visual check
- **OBS Studio** with the NDI plugin
- NDI Advanced SDK example receivers in `/Library/NDI Advanced SDK for Apple/examples/C++/` (`NDIlib_Recv`, `NDIlib_Recv_HDR`, `NDIlib_Jitter_Measure`, `NDIlib_Latency_Test`) — useful for programmatic/HDR validation

## Troubleshooting

**Plugin doesn't appear in Resolve**
1. Confirm the bundle is installed: `ls "/Library/OFX/Plugins/NDIOutput.ofx.bundle/Contents/macOS/"`
2. Check the dylib is resolvable: `otool -L "/Library/OFX/Plugins/NDIOutput.ofx.bundle/Contents/macOS/NDIOutput.ofx"` — the NDI entry must be an absolute path, not `@rpath` (if it's `@rpath`, `make install`'s `install_name_tool` step didn't run)
3. Check the plugin cache entry for `status="0"` (command above). If the entry is stale or bad, quit Resolve and delete `OFXPluginCacheV2.xml` to force a full re-scan at next launch.

**Plugin loads but no NDI source on the network**
1. "Enable NDI Output" checked in the plugin?
2. Timeline actually rendering? Smart/Render Cache and proxy modes suppress or alter OFX render calls — see the pitfalls list in [LEARNINGS.md](LEARNINGS.md)
3. Firewall/network: verify other NDI sources are visible (NDI Test Patterns.app is a good known-good sender)

**Live plugin logs** (the plugin logs via `os_log`, prefixed `NDI Plugin:`):
```bash
./scripts/monitor_ndi_logs.sh
```
