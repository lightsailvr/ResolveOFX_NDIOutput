# Spec: Windows Port of the NDI Output OFX Plugin

**Status:** approved plan · **Branch:** `windows-port` (long-lived integration branch off `dev`) · **Date:** 2026-08-30
**Source research:** [2026-08-30-windows-port-feasibility.md](2026-08-30-windows-port-feasibility.md) — every decision below traces to a verified finding there.
**Tickets:** GitHub issues (labels `windows` + triage labels), each referencing this spec and the `windows-port` branch.

## Problem Statement

Editors and colorists running DaVinci Resolve on Windows cannot use the NDI Output plugin at all — it ships for macOS only. Windows is the dominant Resolve platform for NVIDIA-based immersive workstations, so the users with the most GPU headroom for 8K stereo monitoring are exactly the users the plugin excludes today. A Windows attempt from May 2025 never built, was never diagnosed, and now predates the plugin's entire modern architecture (sender hub, eye pairer, async pump, fused GPU pipeline, STMap warp), so there is nothing a Windows user can even build from source.

## Solution

Ship a first-class Windows version of the plugin as part of the same product: one codebase, one version number, one release event with per-platform artifacts. Windows users download a standard installer from the GitHub release page, run it, restart Resolve, and get the same NDI Output effect macOS users have — SDR and HDR, stereo pairing, STMap dewarp, resolution controls — GPU-native on NVIDIA hardware via CUDA, with the existing CPU path as the automatic fallback on non-NVIDIA machines. The port is developed on a dedicated long-lived branch with its own build pipeline (CMake + MSVC + CUDA, compile-only CI, Inno Setup installer) and merges into `dev` when it passes the same tiered testing loop that gates macOS work.

## User Stories

1. As a Windows Resolve editor, I want to add the NDI Output effect to my timeline and see the program feed appear as an NDI source on my network, so that I can monitor my edit on any NDI receiver without extra hardware.
2. As a Windows Resolve user on the free (non-Studio) version, I want the plugin to load and stream, so that I don't need a Studio license just for NDI monitoring.
3. As a colorist grading HDR on Windows, I want the stream to carry HDR (Rec.2020/BT.2100) exactly as the macOS build does, so that my NDI monitor shows what the grade actually looks like.
4. As an immersive editor cutting 8K stereo on a Windows NVIDIA workstation, I want eye pairing, packing, downscale, and color conversion to run GPU-native, so that playback stays real-time instead of collapsing under full-resolution CPU readback.
5. As an immersive editor, I want STMap/lens-map dewarp on Windows at parity with macOS, so that fisheye footage previews correctly on my monitoring feed.
6. As a stereo editor, I want the eye pairer and canvas guard to behave identically on Windows, so that mismatched or mono canvases degrade exactly as they do on macOS.
7. As a Windows user with an AMD or Intel GPU, I want the plugin to fall back to the CPU path automatically with a documented resolution ceiling, so that I still get a working HD/UHD stream instead of a broken plugin.
8. As a user whose NVIDIA driver is too old for the plugin's CUDA runtime, I want the plugin to degrade to the CPU path with a status message rather than fail, so that older machines stay usable.
9. As a Windows user, I want the resolution controls (Full/Half/Quarter) to behave identically to macOS, so that I can trade bandwidth for fidelity the same way on both platforms.
10. As a user installing the plugin, I want a Windows installer that puts everything in the right place and registers an uninstaller, so that install and removal are one double-click each.
11. As a user with no NDI software installed, I want the plugin to work out of the box, so that I never have to find and install an NDI runtime separately.
12. As a studio IT admin, I want a silent-install flag on the installer, so that I can deploy the plugin across workstations unattended.
13. As a user downloading an unsigned installer, I want the SmartScreen "More info → Run anyway" flow documented in the README and release notes, so that the warning reads as a known step, not malware.
14. As a first-time Windows user, I want the Windows Firewall prompt on first NDI send documented, so that my stream isn't silently invisible on the network.
15. As a user whose plugin vanished from the Effects Library, I want a documented Windows plugin-cache reset procedure, so that I can recover without filing an issue.
16. As a user reporting a problem, I want live plugin logs viewable on Windows with a documented tool, so that I can capture diagnostics for a bug report.
17. As the maintainer, I want compile-only Windows CI on every push to the port branch, so that the Windows build can never again silently rot between sessions.
18. As the maintainer, I want the portable unit suite running under MSVC in CI, so that platform-neutral regressions surface without Windows hardware in the loop.
19. As the maintainer, I want the CUDA kernels proven byte-identical to the shared CPU references by automated tests, so that both platforms provably render the same bytes for the same frames.
20. As the maintainer, I want one `VERSION` and one `CHANGELOG.md` across platforms, so that a release stays a single event with per-platform artifacts.
21. As the maintainer, I want Windows artifacts (installer, bare-bundle zip, checksums) attached by the same publish flow as the macOS pkg, so that releasing stays a two-script operation.
22. As the maintainer, I want the NDI attribution and licensing obligations (bundled licenses file, ndi.video link, trademark line, ≤30-day-old SDK at release) honored in the Windows artifacts, so that redistribution stays compliant.
23. As a user of features that ship later (Browse buttons, Timeline (Auto), Camera Metadata projection), I want Stream Status and the release notes to say plainly they are not on Windows yet, so that a missing feature reads as roadmap, not a bug.
24. As an agent or contributor picking up a ticket, I want the branch, build, and testing rules documented on the `windows-port` branch itself, so that Windows work follows the same discipline as macOS work.

## Implementation Decisions

**Product shape**

1. **One codebase, one version — Windows gets its own build pipeline, never its own versioning.** `VERSION` and `CHANGELOG.md` stay unified; release artifacts are platform-suffixed. Rationale: a separately-versioned Windows line would fork the product's identity, double the release bookkeeping, and let the platforms drift; the plugin already displays a single version parameter. A release may ship with one platform lagging (noted in release notes) — that is a release-notes fact, not a version fork.
2. **Branch model:** `windows-port` is a long-lived integration branch off `dev`. Windows work happens on `feature/win-<name>` branches off `windows-port`, merged into `windows-port` by PR once the Windows testing loop passes. When the port meets this spec's release bar, `windows-port` merges into `dev` by PR like any feature; releases remain `dev` → `master`, only on Matt's explicit instruction. Never commit to `master` (unchanged).
3. **Release bar for the first Windows release:** installer-to-visible-stream on a fresh machine, GPU-native CUDA path at 8K stereo, and the deferred-features list stated in release notes.

**GPU strategy**

4. **CUDA-native fused pipeline on Windows (NVIDIA), CPU path as the universal fallback, OpenCL deferred.** The plugin declares CUDA render + CUDA-stream support on Windows only; on non-NVIDIA machines the host hands CPU buffers and the existing platform-neutral CPU path renders (HD/UHD-viable, with a documented ceiling — full-res float readback makes 8K stereo infeasible on CPU buffers by arithmetic, same as macOS before the Metal-native path). OpenCL for AMD/Intel full-resolution support is consciously deferred until demand exists, because it would triple every future kernel change.
5. **The CUDA module implements the existing GPU-module contract** — the same header-level C API the Metal module implements (fused downscale+convert kernels for UYVY and P216, their STMap-warp variants, the non-blocking slot-ring submit with completion callbacks, device-to-device passthrough copy, device readback, per-device map-upload cache). The async pump un-gates from Apple-only to any GPU-native platform and consumes CUDA completions identically.
6. **CUDA discipline (host contract):** enqueue all work on the host-provided CUDA stream; never block the render action on GPU completion; never call `cudaDeviceSynchronize`; never call `cudaDeviceReset` inside the host process. Completions fire via host-function callbacks on the host's stream; staging slots are pinned host memory; GPU timing via CUDA events.
7. **The May-2025 scaffolding is deleted, not repaired**: the host-memory CUDA sketch, the D3D11 "fallback" that converts nothing, the vestigial OpenGL includes, the MinGW build script (CUDA on Windows requires MSVC as host compiler), and the aspirational Windows README that documents behavior that never existed.

**Toolchain & CI**

8. **CMake is the Windows build harness** (first-class CUDA language support), driven by Visual Studio 2022 and **CUDA Toolkit 12.9** (pinned: keeps GTX 10-series/Pascal buildable, matches the `windows-2022` CI image, and sits on Resolve 21's own Windows CUDA floor). CUDA architectures: Pascal + Turing + Ampere as real targets plus PTX from the newest for forward compatibility. zlib comes from vcpkg. Whether CMake later also drives the macOS build is a separate decision; until then the Makefile remains the macOS harness and BUILD.md is the contract between them.
9. **GitHub Actions CI on `windows-2022`, compile-only:** configure + build the full plugin (including CUDA compilation via a toolkit-install action), run the portable unit suite under MSVC, build the installer, upload artifacts. CI cannot execute kernels, load Resolve, or verify a stream — Tiers 1–2 stay human, on real hardware. Workflow triggers include `windows-port` pushes and PRs.
10. **A Windows machine with an NVIDIA GPU is a standing prerequisite** — CUDA cannot be cross-compiled from macOS, and every release's Tier 1–2 pass happens on that machine.

**NDI runtime & compliance**

11. **Bundle the NDI runtime DLL inside the bundle** (the vendor-recommended mechanism, mirroring the macOS dylib-in-Frameworks approach) rather than requiring the NDI runtime installer. The Windows **Advanced** SDK remains required — the standard SDK cannot send HDR.
12. **Delay-load the NDI import and add the bundle's binary directory to the DLL search path at startup** (module-relative, resolved from the plugin's own module handle). Windows' default loader never searches a DLL's own folder for load-time imports of another DLL, so without this the plugin silently fails to load on any machine without an NDI runtime — the exact machine the bundled DLL exists for. Verified on a machine with no NDI software installed.
13. **Attribution ships in the artifacts:** the NDI third-party-licenses file beside the binaries, the ndi.video link and the "NDI® is a registered trademark of Vizrt NDI AB" line in the installer readme and release notes, and the SDK no more than 30 days old at each release (same obligations the macOS pkg already meets).

**Windows plumbing**

14. **Logging:** the plugin's logging macro routes to `OutputDebugStringA` plus an optional append-only file sink on Windows (live capture via DebugView/WinDbg), replacing the macOS `os_log` path so the Tier-2 loop keeps live logs.
15. **Unicode paths:** every file-touching seam (STMap open/stat, EXR reader, later the BRAW path) converts UTF-8 parameter strings to UTF-16 and uses wide CRT calls, so non-ASCII paths work.
16. **Packaging layout is the OFX spec's:** `NDIOutput.ofx.bundle/Contents/Win64/NDIOutput.ofx` installed under the Common Files OFX plugins directory (the flat install in the old scaffold was wrong).
17. **Installer: Inno Setup** — one small script; admin elevation by default, automatic uninstaller and Add/Remove entry, `/VERYSILENT` support, built-in signing hook, preinstalled on the CI image. WiX/MSI is the revisit-if-studios-ask-for-GPO option; NSIS adds only hand-written uninstall logic.
18. **Ship v1 unsigned, with the SmartScreen click-through documented**; set up Azure Artifact Signing (~$10/month) the moment install-friction reports appear. Do not buy an EV certificate — current Microsoft documentation states EV no longer bypasses SmartScreen.
19. **Deferred features ship later, behind their existing soft-fails:** Browse buttons (native file dialog), Timeline (Auto) camera-clip watcher (needs a process-spawn port and the Windows Resolve scripting paths), and Camera Metadata (BRAW) projection (needs the Windows dispatch shim and a MIDL build step). Each already degrades gracefully with a Stream Status message; Windows release notes state what's pending. Manual Path STMap workflows are in scope for v1.

## Testing Decisions

- **The seam is the existing GPU-module contract; no new seams.** The plugin core talks to GPU acceleration through one header-level C API today (implemented by the Metal module); the CUDA module implements the same API. This is the highest existing seam and the ideal count (one). All pairing/packing/policy logic stays in the platform-neutral layer above the seam, where it is already tested.
- **Good tests assert external behavior at the seam: bytes out for frames in.** The shared CPU reference implementations (downscale, warp, converters) are the oracles. The CUDA kernels must be **byte-identical** to the CPU references, including the invariant that an identity STMap reproduces the plain downscale kernels exactly — this mirrors the existing Metal kernel-identity test, which is the prior art to copy.
- **The portable unit suite runs under MSVC in CI on every push** — eye pairer, packers, canvas guard, stream-resolution math, EXR/STMap reader (fixtures + hostile inputs), calibration-JSON/lens-map golden values. Unchanged tests, new platform; they need neither Resolve nor the NDI SDK nor a GPU.
- **The tiered testing loop transplants** (documented in CLAUDE.md/BUILD.md on this branch): Tier 0 = CMake build + unit tests (CI-runnable); Tier 1–2 = elevated install of the bundle, full Resolve restart, human-started playback, stream verified in NDI Tools' Studio Monitor — from a second machine, so firewall behavior is part of the test. Windows plugin cache resides under `%APPDATA%` (with an extra `Support\` level); same delete-to-rescan procedure as macOS.
- **Host-behavior probes re-run on Windows before trusting them:** the render-call probe matrix (Render Cache, proxy modes, stereo per-eye instances) is assumed identical to macOS only after the Log Render Calls probe confirms it.
- **The NDI-DLL loading test runs on a machine with no NDI software installed** — that machine is the end-user machine the bundled-DLL decision exists for.
- **The installer test is release-shaped:** fresh Windows machine, download from a draft GitHub release, install, restart Resolve, see the stream; uninstall removes cleanly; silent install works.

## Out of Scope

- **An OpenCL backend** for full-resolution AMD/Intel GPU support (deferred until demand; CPU fallback covers those users at HD/UHD).
- **Windows on ARM** (Resolve runs ARM64EC there; no CUDA exists for it — explicitly unsupported in release notes).
- **Code-signing purchase for v1** (unsigned with documented SmartScreen flow; Azure Artifact Signing is the upgrade path, not a launch requirement).
- **Separate Windows version numbers or a Windows-specific changelog** (decision 1).
- **Migrating the macOS build to CMake** (worth considering after the port lands; not part of it).
- **Linux** (the old scaffold's Linux stub stays a stub).
- **NDI vendor ID** (free plugin; not required for non-commercial distribution — unchanged from macOS).
- **Transport control from the plugin** (Resolve's API has none; playback stays human-started on every platform).

## Further Notes

- **Two licensing items are open, both platform-neutral and started in the first ticket:** the Windows Advanced SDK download is access-gated (request early), and the Advanced-SDK "trial" HDR 30-minute wording deserves a definitive answer from NDI licensing before the first Windows release (the macOS releases already live under whatever this status is — worst case, HDR mode gets a documented caveat while SDR is unaffected).
- **The effort estimate from the feasibility study is ~2.5–4.5 focused weeks** to the first Windows release (CUDA path + installer), plus an optional parity tail — assuming the hardware exists.
- **Seam check (for Matt):** this spec deliberately adds no new seams — the CUDA module implements the same GPU-module contract the Metal module does, and everything above that contract stays shared. If you want a different cut (e.g., a thinner per-kernel seam, or pushing the pump below the contract), say so before the CUDA ticket starts; it is the one place the port's shape could still change cheaply.
- The fictional Windows README from the May-2025 attempt is replaced on this branch by a short status pointer; BUILD.md's Windows section and CLAUDE.md carry the working rules so any session on the Windows machine inherits them.
