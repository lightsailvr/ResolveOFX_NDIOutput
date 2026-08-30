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

The plugin links against zlib (`-lz`, for the STMap EXR reader's Zip-compressed chunks) and, on macOS, AppKit + UniformTypeIdentifiers (for the Browse buttons' native open panel, `src/MacFileDialog.mm`). All ship with the macOS SDK — no extra install. A future Windows build needs a zlib to match; the browse buttons are macOS-only.

The Camera Metadata projection (v1.10.0) compiles against [third_party/braw/](third_party/braw/) — the Blackmagic RAW API header and dispatch shim, vendored from Blackmagic RAW SDK 5.1 under their Boost-style license (notices intact; re-copy both files from `/Applications/Blackmagic RAW/Blackmagic RAW SDK/Mac/Include/` to update). **Nothing Blackmagic is linked or bundled**: the shim resolves `BlackmagicRawAPI.framework` at runtime from the host application's own bundle — inside Resolve that is Resolve's shipped copy (identical to the SDK's, verified 5.1/50100.40.160) — falling back to the standalone SDK install, and failing soft (passthrough + Stream Status message) when neither exists. Building the plugin therefore needs no Blackmagic install at all.

Its Timeline (Auto) camera-clip source (v1.11.0) bundles `src/ndi_timeline_watch.py` into `Contents/Resources/` (the Makefile copies it); at runtime the plugin spawns it with the system python3 to poll the Resolve scripting API for the clip under the playhead (`src/TimelineClipWatcher.cpp`). Auto mode needs Resolve's external-scripting preference (Preferences → System → General → External scripting using: Local) and a python3 on the machine; without either it logs why and Manual Path mode still works.

### Unit tests

```bash
make test
```

Host-independent tests in [tests/](tests/) (render-probe log-line formatter, stream-resolution divisor/dims/box-downscale, the stereo eye-pairer/packers — pairing decisions, reorder window, starvation fallback, SbS/TB byte layout — the STMap seam: the EXR reader against both third-party fixture files in [tests/fixtures/](tests/fixtures/) and in-test-built ones incl. hostile inputs, plus the CPU reference warp — and the camera-metadata seam: the calibration-JSON parser and Mei-Rives map generator against golden values from an independently verified implementation, incl. hostile/locale cases) — compiles in seconds into `build/`, needs neither Resolve nor the NDI SDK (the brawmap test uses only the SDK-free `src/BRAWLensMap.h`). Run it alongside `make dev` before installing.

The `tests/fixtures/*.exr` files were written by ffmpeg's OpenEXR encoder (32×16 identity STMap; one file per compression, plus a half-float variant) so the EXR reader is validated against files our own test writer didn't author. `tests/fixtures/ursa_immersive_calibration.json` is the real `OpticalProjectionData` blob from an URSA Cine Immersive clip (see `docs/2026-08-30-braw-immersive-metadata-projection.md`).

```bash
make test-metal
```

GPU kernel correctness tests: runs the fused downscale+convert Metal kernels (UYVY and P216) and their STMap-warp variants (v1.7.0) on a synthetic frame — through both the blocking calls and the non-blocking slot-ring submit path (v1.6.0) — and compares against the CPU references (`ndi_stream::downscaleRGBABox`, `ndi_stmap::warpRGBABox`, plus the converters). An identity STMap must reproduce the plain downscale kernels byte-for-byte. Needs a Metal device (any Mac; skips cleanly without one), but still no Resolve or NDI SDK. The kernels are compiled from source at runtime, so this is the only pre-Resolve check that catches shader errors — run it whenever `src/MetalGPUAcceleration.mm` changes.

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

### Release packaging (signed installer + GitHub release)

Two scripts turn a working tree into a public release:

```bash
./scripts/package_release.sh          # build → sign → pkg → notarize → dist/v<VERSION>/
./scripts/publish_github_release.sh   # tag + draft GitHub release with the artifacts
```

`package_release.sh` produces a **universal (arm64 + x86_64), macOS 13+ binary** with `libndi_advanced.dylib` bundled inside `Contents/Frameworks/` (`@loader_path` reference) — end users need **no NDI SDK**, unlike the dev flow where `make install` points the plugin at the SDK's dylib by absolute path. It signs the bundle with the Developer ID **Application** cert (hardened runtime, timestamps), builds a `productbuild` pkg installing to `/Library/OFX/Plugins` (bundle relocation disabled), signs that with the Developer ID **Installer** cert, then notarizes and staples via `notarytool`. Output: `NDIOutput-<V>-macOS.pkg` (installer), `NDIOutput-<V>-macOS.zip` (bare signed bundle for manual installs), `SHA256SUMS.txt`. The NDI attribution file `libndi_licenses.txt` ships inside `Contents/Resources/` and the installer readme carries the required ndi.video link (NDI SDK distribution terms).

One-time signing setup (the script's preflight names anything missing):

1. **Developer ID Application** certificate in the login keychain.
2. **Developer ID Installer** certificate — create in Xcode → Settings → Accounts → Manage Certificates → **+** → Developer ID Installer (Account Holder only), or via CSR at developer.apple.com.
3. **Notarization profile** — one-time, with an app-specific password from [account.apple.com](https://account.apple.com) → Sign-In and Security → App-Specific Passwords:
   ```bash
   xcrun notarytool store-credentials NDI_NOTARY --apple-id <your-apple-id> --team-id <TEAMID>
   ```

Flags: `--skip-notarize` (local testing), `--skip-tests`, `--host-arch-only`, `--unsigned-dev-build` (pipeline smoke test without the Installer cert — output named `-UNSIGNED-DEV`, never distribute it).

`publish_github_release.sh` refuses un-notarized pkgs (`stapler validate` + `spctl` gate), takes release notes from the `## [X.Y.Z]` section of CHANGELOG.md (or `--notes-file`), pushes the `vX.Y.Z` tag, and creates a **draft** release — publish after review with `gh release edit vX.Y.Z --draft=false` (or run with `--publish`).

A release build differs from the dev build (universal, deployment target, bundled dylib), so run the Tiers 1–2 loop with the **pkg-installed** plugin before publishing.

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
1. Confirm the bundle is installed: `ls "/Library/OFX/Plugins/NDIOutput.ofx.bundle/Contents/MacOS/"`
2. Check the dylib is resolvable: `otool -L "/Library/OFX/Plugins/NDIOutput.ofx.bundle/Contents/MacOS/NDIOutput.ofx"` — the NDI entry must be an absolute path, not `@rpath` (if it's `@rpath`, `make install`'s `install_name_tool` step didn't run)
3. Check the plugin cache entry for `status="0"` (command above). If the entry is stale or bad, quit Resolve and delete `OFXPluginCacheV2.xml` to force a full re-scan at next launch.

**Plugin loads but no NDI source on the network**
1. "Enable NDI Output" checked in the plugin?
2. Timeline actually rendering? Smart/Render Cache and proxy modes suppress or alter OFX render calls — see the pitfalls list in [LEARNINGS.md](LEARNINGS.md)
3. Firewall/network: verify other NDI sources are visible (NDI Test Patterns.app is a good known-good sender)

**Live plugin logs** (the plugin logs via `os_log`, prefixed `NDI Plugin:`):
```bash
./scripts/monitor_ndi_logs.sh
```
