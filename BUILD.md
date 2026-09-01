# Build & Installation Guide

Canonical instructions for building, installing, and verifying the NDI Output OFX plugin. If a build change lands, this file must be updated in the same PR.

- **macOS** — primary platform, working (Metal GPU path)
- **Windows** — port in progress on branch `windows-port`: CUDA GPU-native pipeline with kernel identity tests (ticket #22), native Browse dialogs (ticket #24), and the Timeline (Auto) clip watcher (ticket #25) in the build; installer pending (see [status](#windows--status-cuda-pipeline-22-browse-dialogs-24-and-the-timeline-auto-watcher-25-in-the-build-installer-pending-23))

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

The plugin links against zlib (`-lz`, for the STMap EXR reader's Zip-compressed chunks) and, on macOS, AppKit + UniformTypeIdentifiers (for the Browse buttons' native open panel, `src/MacFileDialog.mm`). All ship with the macOS SDK — no extra install. The Windows build gets its zlib from vcpkg and its Browse dialogs from `src/WinFileDialog.cpp` (IFileOpenDialog; ole32 + shell32) — see the Windows section.

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

The version lives in **three places that must stay in sync**: the `VERSION` file, the `#define kPluginVersion*` macros in [NDIOutputPlugin.cpp](src/NDIOutputPlugin.cpp), and `Info.plist` (`CFBundleShortVersionString`/`CFBundleVersion` — a stale value here once made the pkg installer silently skip the bundle; see LEARNINGS 2026-08-30). Always bump with the scripts, which update all three:

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

## Windows — STATUS: CUDA pipeline (#22), Browse dialogs (#24), and the Timeline (Auto) watcher (#25) in the build; installer pending (#23)

The May-2025 scaffold (commit `50eacc1`) never built, was never diagnosed, and predated the plugin's modern architecture — the port is being **redone, not repaired**, on the long-lived `windows-port` branch (its dead pieces — the MinGW script, the D3D11 "fallback", the OpenGL vestiges, the host-memory CUDA sketch — are deleted; git history keeps them). Plan and decisions: [docs/windows-port-spec.md](docs/windows-port-spec.md); research: [docs/2026-08-30-windows-port-feasibility.md](docs/2026-08-30-windows-port-feasibility.md); work items: GitHub issues labeled `windows`. Findings still go to [LEARNINGS.md](LEARNINGS.md).

**Still owed from issue #22 (Tier 1–2, human on the workstation):** GPU-native log lines during real playback with no CPU-fallback lines, 8K stereo rates comparable to macOS, and the render-call probe matrix re-run (Render Cache / proxy modes / stereo per-eye instances) before trusting stereo pairing on Windows — findings to LEARNINGS.md when they happen. Tier 0 (build + all tests, kernel identity on the workstation GPU) is what this section's status line covers.

**Still owed from issue #24 (Tier 1–2, human in Resolve):** the four Browse buttons appear next to their path fields, a picked file (including a non-ASCII path) lands in the field and the map/clip loads, and Cancel changes nothing — findings to LEARNINGS.md.

**Still owed from issue #25 (Tier 1–2, human in Resolve):** with Camera Metadata + Timeline (Auto) selected and playback running, the DebugView log shows `TimelineWatch: playhead clip: '<path>'` lines following cuts across a multi-clip timeline; disabling external scripting (or renaming python) produces the documented soft-fail lines and Manual Path keeps working. (The end-to-end projection itself — clip path → lens maps → warped stream — additionally needs the BRAW reader port, ticket #26; until then Auto mode on Windows reports and logs the followed clip but map generation states "Camera-metadata projection is macOS-only for now".) The helper's scripting chain (module import from `%PROGRAMDATA%`, `fusionscript.dll` hand-off, playhead query against live Resolve) was verified standalone on the workstation 2026-08-31.

### Prerequisites

- **Visual Studio 2022** with the "Desktop development with C++" workload (MSVC v143 + Windows SDK)
- **CMake** ≥ 3.21 and **vcpkg** (supplies zlib via [vcpkg.json](vcpkg.json))
- **Windows NDI 6 Advanced SDK** installed at `C:\Program Files\NDI\NDI 6 Advanced SDK` (or pass `-DNDI_SDK_PATH=...`). HDR needs Advanced; the download is access-gated, request early. Without it the build automatically links a **stub import library** built from [third_party/ndi/](third_party/ndi/) — good for compile-proof only; a streaming binary needs the real SDK.
- **CUDA Toolkit 12.9** (pinned; spec decision 8) — compiles the GPU-native pipeline (`src/CudaGPUAcceleration.cu`; nvcc requires MSVC as host compiler — that constraint killed the old MinGW script). Nothing can be cross-compiled from macOS. `-DNDI_ENABLE_CUDA=OFF` builds the CPU-only plugin without the toolkit (no GPU-native path — the documented non-NVIDIA behavior, not a shippable default).

### Build (Tier 0)

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -T "cuda=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.9" -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
```

(`$VCPKG_ROOT` = your vcpkg checkout; the GitHub runner preinstalls one and exposes it as `VCPKG_INSTALLATION_ROOT`, which is what the workflow passes. The `-T "cuda=..."` toolset points the VS generator straight at the CUDA toolkit — required whenever the CUDA **Visual Studio integration** was never copied into the VS installation, an admin-only step the full toolkit installer performs but a scripted/partial install does not; without either, configure fails with `No CUDA toolset found`. Harmless when the integration IS installed, so just always pass it.)

```bash
cmake --build build --config Release --parallel
```

```bash
ctest --test-dir build -C Release --output-on-failure
```

`ctest` runs the portable unit suite from `make test` on macOS — including `test_platform_paths` (UTF-8⇄UTF-16 path shims) and `test_ndi_loader` (NDI runtime path derivation) — plus three Windows-side additions the Makefile doesn't build: `test_win_file_dialog` (the browse dialog's pure logic, ticket #24), `test_timeline_watch` (the watcher's Python-discovery/quoting/path seams plus a live pipe-spawn smoke test, ticket #25), and `test_plugin_delayload`, which loads the built `.ofx` with a System32-only import search (what Resolve's plugin scanner amounts to), so any stray load-time import fails Tier 0 instead of silently emptying the Effects Library. CMake refuses to configure if `VERSION` and `kPluginVersionString` in `src/NDIOutputPlugin.cpp` disagree — run `scripts/set_version.sh`, never edit either by hand.

`test_cuda_downscale` (ticket #22) is the Windows counterpart of `make test-metal`: it holds the fused CUDA kernels **byte-identical** to the shared CPU references (`ndi_stream::downscaleRGBABox`, `ndi_stmap::warpRGBABox`, the flipping converters), including identity-STMap ≡ plain-downscale, plus the slot ring and passthrough/readback contracts. It needs a CUDA device — it runs for real on the workstation and skips cleanly on GPU-less machines (hosted CI compiles it, which is CI's whole job here). Byte-identity leans on `--fmad=false` for the CUDA translation units (CMakeLists.txt) — don't "optimize" that flag away.

### Install (Tier 1)

```bash
cmake --install build --config Release --prefix stage
```

produces the spec bundle tree `stage/NDIOutput.ofx.bundle/Contents/Win64/NDIOutput.ofx` (with `Processing.NDI.Lib.Advanced.x64.dll` + its licenses file beside the binary when the real SDK is present — a stub-linked CI-style build stages no DLL and will not stream). Then, from an **elevated** PowerShell (UAC prompts spawned by automation shells can auto-cancel; open the terminal elevated yourself):

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\install_windows.ps1              # copies the bundle into C:\Program Files\Common Files\OFX\Plugins
powershell -ExecutionPolicy Bypass -File .\scripts\install_windows.ps1 -ResetCache  # same, plus force a full plugin-cache re-scan
```

(The `-ExecutionPolicy Bypass -File` wrapper is required on any machine with the default `Restricted` policy — a bare `.\scripts\install_windows.ps1` fails with "running scripts is disabled on this system". It's per-invocation; don't change the policy system-wide.)

The script refuses to run while Resolve is open (no OFX hot reload). Fully restart Resolve afterwards and verify the stream in **Studio Monitor** (free NDI Tools) — **from a second machine**, so firewall behavior is part of the test.

**How the NDI runtime resolves (ticket #21):** the DLL ships inside the bundle, but Windows never searches a DLL's own folder for its load-time imports — a plain import would silently drop the plugin from the Effects Library on any machine without a system NDI runtime. So the import is **delay-loaded** (CMakeLists.txt) and the plugin **preloads the DLL by full module-relative path** before the first NDI call (`src/NDIRuntimeLoader.h`), falling back to a system-wide NDI runtime, and disabling streaming (plugin still loads, passes frames through) if neither exists. `test_plugin_delayload` guards all of this in CI, including that no other load-time import outside System32 ever creeps in (see LEARNINGS.md: the `z.dll` trap). **Release-bar check:** verify once on a machine with *no* NDI software installed and nothing NDI-related on `PATH` — that machine is who the bundled DLL exists for.

**Firewall:** the first NDI send triggers the Windows Firewall prompt for the *Resolve* process. Allow on private networks; decline (or a silently-dropped prompt, e.g. non-interactive sessions) leaves the source discoverable but the video unreachable from other machines — the classic "Studio Monitor lists it but shows black" symptom. Verify reachability from a second machine, not localhost.

Plugin cache on Windows: `%APPDATA%\Blackmagic Design\DaVinci Resolve\Support\OFXPluginCacheV2.xml` (note the extra `Support\` level vs macOS; same delete-with-Resolve-closed rescan procedure, or `-ResetCache` above). If the plugin doesn't appear in the Effects Library: reset the cache first, then watch the load in DebugView (below) — the loader logs which NDI runtime path it resolved (bundle, system, or NOT FOUND with both Win32 error codes).

### Timeline (Auto) camera-clip watcher (ticket #25)

The Windows counterpart of the macOS playhead watcher (BUILD.md macOS section, v1.11.0): the plugin spawns the bundled `Contents/Resources/ndi_timeline_watch.py` as a hidden console process (`src/WinTimelineWatch.cpp`; spawn/discovery seams and their tests in `src/WinTimelineWatch.h`) and the helper polls the Resolve scripting API ~2×/s for the clip under the playhead. Requirements on Windows:

- **Resolve Studio with external scripting enabled** — Preferences → System → General → *External scripting using: Local*. Without it the helper connects to nothing and the log says `scriptapp('Resolve') returned nothing — is external scripting enabled?`.
- **A 64-bit Python 3.** Discovery order (the plugin logs which one it picked): the **PEP 514 registry** — `Software\Python\PythonCore` under HKCU then HKLM, 64-bit view, skipping `-32`/`-arm64` tags, highest version wins — then a **PATH search** for `python.exe` as fallback. The python.org x64 installer is the recommended install (it registers under PEP 514 whether or not "Add to PATH" was checked); the Microsoft Store Python also registers and is verified working on the dev workstation (3.12). Resolve's own scripting docs assume a python.org install, so prefer that on user machines.

The helper finds Resolve's scripting environment without configuration: the scripting modules load from `%PROGRAMDATA%\Blackmagic Design\DaVinci Resolve\Support\Developer\Scripting\Modules`, and the plugin derives the `fusionscript.dll` path from the *running* `Resolve.exe` (the watcher spawns from inside Resolve's process) and hands it to the helper, so non-default install directories work.

Every failure is soft and named in the log (DebugView filter `NDI Plugin: TimelineWatch:`): no Python found, helper script missing from the bundle, spawn failure, scripting disabled, module-import failure. The helper respawns with 30 s backoff; Manual Path mode is unaffected throughout. When Auto mode has no usable clip, Stream Status shows the generic camera-metadata passthrough message (same text as macOS) and the *log* carries the watcher's health detail alongside it.

### Logs on Windows

`NDI_LOG` routes to `OutputDebugStringA` — watch live with [DebugView](https://learn.microsoft.com/en-us/sysinternals/downloads/debugview) (filter `NDI Plugin:`) or WinDbg. Setting `NDI_OUTPUT_LOG_FILE` to a writable path before launching Resolve additionally appends every line there.

### CI

[.github/workflows/windows.yml](.github/workflows/windows.yml) runs on every push to `windows-port` and any `feature/win-`-prefixed branch (glob `feature/win-**`), on PRs into `windows-port`, and on manual dispatch: install CUDA Toolkit 12.9 (nvcc + cudart + VS integration, network method), configure + build on `windows-2022` **including the CUDA translation units**, the portable unit suite under MSVC, and a bundle-layout check, uploading the staged tree as the `NDIOutput-win64` artifact. CI has no GPU and no Resolve — kernel identity executes on the workstation, and Tiers 1–2 stay human, on real hardware.

Same rule as macOS: any Windows build change updates this section in the same PR.

---

## Receiving the stream (test receivers)

- **NDI Studio Monitor** (Windows, part of free [NDI Tools](https://ndi.video/tools/)) — the Windows-loop receiver; run it on a second machine so the firewall is part of the test
- **NDI Video Monitor** (installed at `/Applications/NDI Video Monitor.app`) — quickest visual check on macOS
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
