# CLAUDE.md

OpenFX plugin for DaVinci Resolve that streams the rendered frame to NDI (SDR + HDR, Metal-accelerated on macOS; Windows/CUDA port in progress and currently not building).

## Workflow rules

- **Never commit to `master`.** Day-to-day base branch is `dev`.
- **Branching/release flow (Matt, 2026-08-30):** every new feature gets its own `feature/<name>` branch off `dev` and merges into `dev` by PR once the testing loop passes. Releases happen only when Matt explicitly instructs: open a release PR merging `dev` → `master` (it must already carry the version bump and a `CHANGELOG.md` `## [X.Y.Z]` section — the publish script reads its release notes from there); after the merge, run `./scripts/package_release.sh` then `./scripts/publish_github_release.sh --publish` from `master` so the GitHub release updates automatically.
- **Testing loop:** LEARNINGS.md §2. Tier 0 (`make dev`) on every change; Tiers 1–2 (install → restart Resolve → verify stream in NDI Video Monitor) before calling anything working. Playback must be started by a human — the Resolve API has no transport control.
- **After fixing any bug or discovering a workflow gotcha, append an entry to LEARNINGS.md** (template at the bottom of that file). This is a standing instruction from Matt.
- **Build/install questions:** BUILD.md is the single source of truth; update it in the same PR as any build change.
- **Version bumps:** only via `scripts/increment_version.sh` / `scripts/set_version.sh` (they sync `VERSION` + the `#define`s in `src/NDIOutputPlugin.cpp`). Never `make bump-*`.

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
