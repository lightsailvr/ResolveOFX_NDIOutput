# CLAUDE.md

OpenFX plugin for DaVinci Resolve that streams the rendered frame to NDI (SDR + HDR, Metal-accelerated on macOS; Windows/CUDA port in the same tree since 2026-09-01, CMake + VS2022 + CUDA 12.9, not yet publicly released — see below).

## Workflow rules

- **Never commit to `master`.** Day-to-day base branch is `dev`.
- **Branching/release flow (Matt, 2026-08-30):** every new feature gets its own `feature/<name>` branch off `dev` and merges into `dev` by PR once the testing loop passes. Releases happen only when Matt explicitly instructs: open a release PR merging `dev` → `master` (it must already carry the version bump and a `CHANGELOG.md` `## [X.Y.Z]` section — the publish script reads its release notes from there); after the merge, run `./scripts/package_release.sh` then `./scripts/publish_github_release.sh --publish` from `master` so the GitHub release updates automatically.
- **Testing loop:** LEARNINGS.md §2. Tier 0 (`make dev`) on every change; Tiers 1–2 (install → restart Resolve → verify stream in NDI Video Monitor) before calling anything working. Playback must be started by a human — the Resolve API has no transport control.
- **After fixing any bug or discovering a workflow gotcha, append an entry to LEARNINGS.md** (template at the bottom of that file). This is a standing instruction from Matt.
- **Build/install questions:** BUILD.md is the single source of truth; update it in the same PR as any build change.
- **Version bumps:** only via `scripts/increment_version.sh` / `scripts/set_version.sh` (they sync `VERSION` + the `#define`s in `src/NDIOutputPlugin.cpp`). Never `make bump-*`.

## Windows port

The Windows port lives in this tree — the long-lived `windows-port` integration branch was merged into `dev` and retired on 2026-09-01 (PR #37). Spec: [docs/windows-port-spec.md](docs/windows-port-spec.md) (decision 2 amended there); research: [docs/2026-08-30-windows-port-feasibility.md](docs/2026-08-30-windows-port-feasibility.md); tickets: GitHub issues labeled `windows`.

- **Branching:** Windows work goes on `feature/win-<name>` branches off `dev`, merged into `dev` by PR once the Windows testing loop passes — same as any feature. The spec's release bar (fresh-machine installer, GPU-native CUDA at 8K stereo, deferred-features list in the notes) gates the unified `dev` → `master` release, not integration into `dev`; a release may ship with one platform lagging, stated in its notes. `master` and release rules are unchanged. No long-lived per-platform branches again — they produced duplicate version bumps and duplicate files against macOS features.
- **Platform isolation:** platform code lives in platform-prefixed files (`src/Win*`, `src/Mac*`, `*.mm`, `*.cu`) behind shared seam headers with host-independent unit tests; gate plugin call sites on capability macros (`NDI_TIMELINE_WATCH`-style) rather than raw `__APPLE__`/`_WIN32` checks. A macOS change must keep the Windows CI compile green and vice versa.
- **One version for both platforms:** `VERSION`/`CHANGELOG.md` stay unified — Windows has its own build pipeline (CMake + VS2022 + CUDA 12.9, compile-only CI on `windows-2022`, Inno Setup installer) but never its own version numbers.
- **Windows testing loop** mirrors LEARNINGS.md §2: Tier 0 = CMake build + unit tests under MSVC (CI runs these on every push); Tiers 1–2 = install the bundle to `C:\Program Files\Common Files\OFX\Plugins\`, fully restart Resolve, verify the stream in NDI Tools' **Studio Monitor** (ideally from a second machine — firewall behavior is part of the test). Playback must still be started by a human.
- **Windows plugin cache:** `%APPDATA%\Blackmagic Design\DaVinci Resolve\Support\OFXPluginCacheV2.xml` (note the extra `Support\` level vs macOS); same delete-it-with-Resolve-closed rescan procedure.
- **GPU rules on Windows:** enqueue kernels on the host-provided CUDA stream; never `cudaDeviceSynchronize`; never `cudaDeviceReset` inside Resolve's process.
- **Live logs on Windows:** `NDI_LOG` routes to `OutputDebugStringA` (+ optional file sink) — watch with DebugView/WinDbg, not `monitor_ndi_logs.sh`.
- The LEARNINGS.md standing rule (append every fixed bug / workflow gotcha) applies to Windows work unchanged, and BUILD.md stays the single source of truth for build/install on both platforms.

## Key commands

```bash
make dev                 # build → NDIOutput.ofx.bundle/
sudo make install        # install to /Library/OFX/Plugins + fix dylib path
./scripts/monitor_ndi_logs.sh   # live plugin logs (os_log, prefix "NDI Plugin:")
```

Resolve must be fully restarted to pick up a new binary (no OFX hot reload). The plugin cache at `~/Library/Application Support/Blackmagic Design/DaVinci Resolve/OFXPluginCacheV2.xml` re-scans on binary mtime/size change; delete it (Resolve closed) if the plugin vanishes from the Effects Library.

## Where things are

- `src/NDIOutputPlugin.cpp` — the whole plugin (OFX C API, no wrapper classes); Metal path in `src/MetalGPUAcceleration.mm`, CUDA port in `src/CudaGPUAcceleration.cu`
- `docs/` — research documents (e.g. the stereo program-tap feasibility study driving the roadmap)
- `LEARNINGS.md` — workflow, testing loop, environment map, solved-bug log
