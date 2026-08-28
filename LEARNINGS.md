# LEARNINGS

Living notebook for this repo: how we branch, how we test, and every bug we've solved together with the rule it taught us. **Append an entry (template at the bottom) whenever a bug is fixed or a workflow assumption turns out to be wrong.**

Fact labels (same convention as the research docs in [docs/](docs/)): **(L)** verified locally on this machine · **(P)** primary source (vendor docs/SDK/headers) · **(U)** user-reported/secondary · **(T)** needs testing.

---

## 1. Branch & release workflow

- **`master`** = stable, released code. Never commit to it directly.
- **`dev`** = integration branch. Day-to-day work happens here; larger features get their own `feature/<name>` branch off `dev`.
- A change reaches `master` **only by PR, only after the testing loop below passes** on the built plugin:
  ```bash
  gh pr create --base master --head dev --title "..." --body "..."
  ```
- Version bumps: `./scripts/increment_version.sh` or `./scripts/set_version.sh` only (they keep `VERSION` and the source `#define`s in sync — the `make bump-*` targets don't; see log entry 2026-08-28).
- `Releases/vX.Y.Z/` currently stores shipped binaries in-repo. Long-term these belong in GitHub Releases so the repo stops growing ~180 KB per release; migrate when convenient.

## 2. The testing loop (researched 2026-08-28)

### The constraint that shapes everything

**Resolve scans OFX plugins once, at startup. There is no hot reload.** Every new binary costs a full Resolve restart (~30–60 s). So the loop is designed to (a) catch as much as possible *before* restarting Resolve, and (b) make the restart cycle as scriptable as possible.

**Plugin cache mechanics** (L — verified by reading `OFXPluginCacheV2.xml`):
- Resolve caches each plugin's entire describe result, keyed on the binary's **`mtime` + `size`**. A rebuilt `.ofx` is re-scanned automatically at the next launch — **no cache clearing needed for normal iteration**.
- `status="0"` on the `<binary>` entry means it described cleanly. If the plugin crashed during load, the entry can go bad and the plugin silently vanishes from the Effects Library. Fix: quit Resolve, delete `OFXPluginCacheV2.xml`, relaunch.
- The cache also records the version Resolve saw — grep it to confirm your build actually got picked up:
  ```bash
  grep -B1 'LSVR.NDIOutput' "$HOME/Library/Application Support/Blackmagic Design/DaVinci Resolve/OFXPluginCacheV2.xml" | head -3
  ```

### Tier 0 — compile (seconds; run on every change)

```bash
make dev
```
Catches compile/link errors without touching Resolve. Iterate here until clean.

### Tier 1 — load smoke test (~1–2 min)

```bash
sudo make install
osascript -e 'quit app "DaVinci Resolve"'; sleep 5; open -a "DaVinci Resolve"
```
**Pass criteria:** cache entry shows the new version with `status="0"`; plugin appears under **LSVR** in the OpenFX panel; version parameter in the plugin UI shows the new build.

### Tier 2 — stream validation (~3 min)

One-time prep: keep a dedicated Resolve project (e.g. **`NDI_PLUGIN_TEST`**) with a short looping timeline, the NDIOutput node applied on the Color page, and **Render Cache off** (Playback → Render Cache → None — see pitfalls below for why this is non-negotiable).

1. Start the log monitor *before* playback: `./scripts/monitor_ndi_logs.sh` (plugin logs via `os_log`, prefix `NDI Plugin:`) (L)
2. Open the test project and press play. Playback cannot be automated — the Resolve scripting API has **no transport control** (L — checked `Developer/Scripting/README.txt`; it can set playhead position via `SetCurrentTimecode` and switch pages, but not play).
3. Verify in **NDI Video Monitor.app**: source appears under its configured name, correct orientation, correct colors, smooth motion.

**Pass criteria:** source discoverable, frames flowing at timeline fps, logs show the expected path (since v1.6.0 the GPU path logs `GPU-native async: ... enqueued` per render plus `wtimer` lines from the pump worker when Log Render Calls is on; pre-1.6.0 builds logged `GPU-native path:`/`Metal GPU acceleration SUCCESS` — no unexpected CPU-fallback lines either way).

### Tier 3 — feature-specific validation

- **HDR:** the reference receiver is the SDK example `NDIlib_Recv_HDR` (P). Validate FourCC is `P216` and metadata carries `<ndi_color_info primaries="bt_2020" transfer="bt_2100_pq" .../>`. NDI Video Monitor gives the visual check.
- **Latency/jitter:** SDK examples `NDIlib_Latency_Test`, `NDIlib_Jitter_Measure` in `/Library/NDI Advanced SDK for Apple/examples/C++/`.
- **GPU path/perf:** `./scripts/monitor_gpu_performance.sh` plus the log lines above; the v1.2.4 release notes list the exact strings that distinguish GPU success from CPU fallback.

### Automation roadmap (not built yet)

- **`ndi_verify` CLI** built from the SDK's `NDIlib_Find` + `NDIlib_Recv` examples: assert source name, resolution, frame rate, FourCC, non-black frames; nonzero exit on failure. Would collapse Tier 2 to one command per test run.
- **`make test`** chaining build → install → Resolve restart → (manual play prompt) → `ndi_verify`.

### Resolve behaviors that will lie to you during testing

From the feasibility research in [docs/2026-08-28-resolve-stereo-program-tap-feasibility.md](docs/2026-08-28-resolve-stereo-program-tap-feasibility.md) §1.2 (P):

| Behavior | What you'll see | Reality |
|---|---|---|
| Smart/Render Cache caches OFX node output | NDI feed **freezes** mid-session | Resolve stopped calling the render action; it's not a plugin bug. Keep Render Cache **off** in the test project |
| Background caching | "Phantom" frames while playback is stopped | Resolve fires renders while parked |
| Timeline Proxy Mode | Stream resolution silently halves/quarters | The plugin receives proxy-sized frames |
| Stereo timelines | Only ever single-eye frames | Probe-verified 2026-08-28 (VR180 4096² and Apple Immersive 8160×7200): a packed L+R frame never reaches OFX. With Stereo 3D palette **Vision: Mono**, only the left eye ever renders; with **Vision: Stereo** (Out: None suffices), both eyes render every frame — **each through its own plugin instance**, pairs sharing `time`/`src` but with **no guaranteed arrival order** at 8K (either eye can lead by up to ~350 ms; in-eye reordering and unmated frames occur). Since v1.5.0 the plugin pairs them itself (cross-instance, time-keyed, reorder-buffered — `src/StereoPair.h`) into one packed SbS/TB stream; the Stream Status parameter shows what's flowing. See docs/2026-08-28-render-call-probe-findings.md |

**Rule: when the feed misbehaves, check these four before suspecting the plugin.**

### Environment map (this machine, verified 2026-08-28)

| What | Where |
|---|---|
| Plugin install dir | `/Library/OFX/Plugins/NDIOutput.ofx.bundle` |
| OFX plugin cache | `~/Library/Application Support/Blackmagic Design/DaVinci Resolve/OFXPluginCacheV2.xml` |
| Resolve app logs | `~/Library/Application Support/Blackmagic Design/DaVinci Resolve/logs/davinci_resolve.log` |
| Plugin runtime logs | macOS unified log → `./scripts/monitor_ndi_logs.sh` |
| Crash reports (plugin takes Resolve down) | `~/Library/Logs/DiagnosticReports/` |
| NDI Advanced SDK | `/Library/NDI Advanced SDK for Apple/` (docs here historically said 6.1.1; the 2026-08-28 feasibility doc reports 6.2.0 installed — confirm with `cat "/Library/NDI Advanced SDK for Apple/Version.txt"` before relying on version-specific behavior) |
| SDK example receivers | `/Library/NDI Advanced SDK for Apple/examples/C++/` |
| SDK prebuilt tools | `/Library/NDI Advanced SDK for Apple/bin/` (Benchmark, DirectoryService, Recording) |
| NDI Tools apps | `/Applications/NDI Video Monitor.app`, `NDI Test Patterns.app`, `NDI Router.app`, … |
| Resolve scripting API | `/Library/Application Support/Blackmagic Design/DaVinci Resolve/Developer/Scripting/` |
| Resolve OFX headers (incl. stereo ext) | `/Library/Application Support/Blackmagic Design/DaVinci Resolve/Developer/OpenFX/` |

---

## 3. Solved bugs & rules learned (running log)

Seeded from release notes and the 2026-08-28 cleanup. Newest entries at the bottom.

### 2025-05-27 — Vertical flip in received stream (v1.1.4)
**Symptom:** NDI receivers showed the picture upside down.
**Root cause:** OFX hands the plugin bottom-up row order; NDI expects top-down.
**Fix:** flip during frame packing (shipped v1.1.4).
**Rule:** any new frame path (new format, new GPU kernel) must re-verify orientation in an actual receiver — it's the cheapest thing to get wrong silently.

### 2025-05-27 — HDR sent black frames (v1.2.3)
**Symptom:** enabling HDR produced black frames and poor performance downstream.
**Root cause:** HDR was being sent as RGBA FourCC; NDI 6 HDR requires **P216** (planar 16-bit 4:2:2) with BT.2100 limited-range quantization and `ndi_color_info` XML metadata.
**Fix:** P216 packing, Rec.2020 coefficients, limited-range encode (Y ∈ [4096, 60160], UV 32768±28672), proper `<ndi_color_info primaries="bt_2020" transfer="bt_2100_pq" matrix="bt_2020"/>` metadata (shipped v1.2.3).
**Rule:** HDR-over-NDI = P216 + metadata, never RGBA. Validate with `NDIlib_Recv_HDR`, not just a visual check.

### 2025-05-27 — GPU silently fell back to CPU; HDR checkbox ignored (v1.2.4)
**Symptom:** Metal acceleration quietly degraded to CPU; toggling HDR mid-session did nothing.
**Root cause:** (1) the HDR Metal shader source was embedded as a string-within-a-string, so shader compilation failed at runtime; (2) `render()` used parameter values cached at `instanceChanged()` time instead of reading them fresh.
**Fix:** shader source integrated properly; parameters read per-render (shipped v1.2.4).
**Rules:** read OFX params fresh inside `render()`; treat any `CPU fallback` log line as a test failure, not a warning — GPU fallback is silent in the UI.

### 2026-08-28 — Repo cleanup findings
- **`make clean` never removed the real objects.** Objects compile into `src/*.o` but clean removed only root `*.o`; two stale 2025 objects were even committed to git. Fixed the target, untracked the files, added `.gitignore`. **Rule:** if a build behaves impossibly, `ls src/*.o` and check dates.
- **Version truth lives in the source `#define`s** ([NDIOutputPlugin.cpp:88-91](src/NDIOutputPlugin.cpp)), maintained only by `scripts/increment_version.sh` / `set_version.sh`. The Makefile injects nothing; CMake's `kPluginVersionString` compile definition is inert because the source `#define` overrides it. `make bump-*` touches only the `VERSION` file → drift. **Rule:** bump via the scripts, nothing else.
- **Docs had drifted badly:** README pointed at v1.1.4 as latest (actual: 1.2.4), required a `/Library/NDI_Advanced_SDK` symlink no build file references, and claimed `make` auto-increments versions (it doesn't); QUICKSTART referenced build scripts deleted in `50eacc1`. **Rule:** [BUILD.md](BUILD.md) is the single source of truth for build/install and gets updated in the same PR as any build change.
- **Bundle layout quirk (T):** we build to `Contents/macOS/` (lowercase) while Resolve's cache records the conventional `Contents/MacOS/`. Works because APFS is case-insensitive here; would break on a case-sensitive volume. Candidate one-line fix in the Makefile — retest plugin load after changing.

### 2026-08-28 — Unified-log visibility gotchas found while building the render probe (v1.3.0)
**Symptom:** two ways diagnostic log lines can silently become unreadable: (1) os_log can redact dynamic `%s` arguments as `<private>` in `log stream`/`log show` depending on system logging config; (2) on this machine's interactive zsh, bare `log` invokes a shell profile function, not `/usr/bin/log` — it fails with "too many arguments".
**Root cause:** (1) unified logging treats dynamic strings as private by default unless the format says `%{public}s` (P — Apple unified-logging docs; L — **confirmed inside Resolve**: the plugin's existing `%s` sender-name log shows `'<private>'` in `log show`, while the same code in an unsigned scratch binary printed plainly — redaction is per-process, so never trust a scratch-binary test for it); (2) a `log` function in the user's shell profile shadows the binary (L).
**Fix:** probe lines and other must-read dynamic strings go through the `NDI_LOG_TEXT` macro (`%{public}s`); `scripts/capture_probe_log.sh` calls `/usr/bin/log` by absolute path.
**Validated by:** Tier 0 + a scratch os_log binary checked with `/usr/bin/log show` (both `%s` and `%{public}s` visible here today).
**Rule:** any log line a human must read for diagnostics uses `%{public}s` (via `NDI_LOG_TEXT`), and scripts/sessions invoke `/usr/bin/log`, never bare `log`.

### 2026-08-28 — Capture script silently overwrote a same-day re-run
**Symptom:** re-running `./scripts/capture_probe_log.sh <slug>` on the same day replaced the previous capture file; the first stereo capture survived only because it was already committed.
**Root cause:** output filename was date+slug only, and `tee` truncates.
**Fix:** the script now suffixes `-2`, `-3`, … when the file exists.
**Validated by:** re-run produces a new file; original untouched.
**Rule:** any evidence-producing script must never overwrite prior evidence — suffix, don't truncate.

### 2026-08-28 — Stereo vision mode blacked out the NDI feed (RESOLVED in v1.5.0)
With the Stereo 3D palette at Vision: Stereo, Resolve creates a **second plugin instance for the right eye**. Both instances created NDI senders with the same source name; the unified log showed `NDIlib_send_create` failing every render in R/L pair cadence, and `initializeNDI`'s failure path called **`NDIlib_destroy()`** — killing the process-wide NDI library under the other instance's healthy sender, so the feed went black and never recovered (L). Duplicate-name registration as the trigger is **confirmed (L, 2026-08-28 v1.4.0 session)**: NDI 6.2 `send_create` fails outright on a name already advertised from the same machine — no auto-suffixing (see the leaked-advertisement entry below).
**Fix (v1.5.0, issue #6):** process-shared, refcounted sender hub — instances sharing a source name share ONE `NDIlib_send` instance (which stereo pairing needs anyway); `NDIlib_initialize()` runs once per process and `NDIlib_destroy()` is never called; sender-create retries throttle to every 3 s with the failure surfaced in the Stream Status parameter.
**Validated by:** Tier 0 + `make test` (pairer seam); Tier 1–2 passed 2026-08-28 (Matt) — stereo and mono both stream correctly on the shared sender.
**Rule:** anything two plugin instances can race over (senders, the NDI library lifetime) must be process-global and refcounted — instance-local ownership of shared OS/network registrations is how the destroy-war started.

### 2026-08-28 — NDI async send vs. send-buffer reallocation (latent, found building the GPU fast path v1.4.0)
**Symptom:** none yet — latent use-after-free. `NDIlib_send_send_video_async_v2` keeps reading the submitted buffer until the *next* send call, but the send buffers (`uyvyFrameBuffer`, `frameBuffer`) were `resize()`d whenever the frame size changed. The new Resolution control (Full/Half/Quarter) makes mid-stream size changes routine, which would have turned the latent bug live.
**Root cause:** NDI async-send buffer contract + `std::vector::resize` reallocation.
**Fix:** `flushAsyncSend()` — a NULL-frame async send completes the in-flight frame — called before any send-buffer reallocation (v1.4.0).
**Validated by:** code inspection against the NDI Advanced SDK async-send contract; Tier 2 re-verify by switching Resolution mid-playback.
**Rule:** any buffer handed to `NDIlib_send_send_video_async_v2` stays untouched until a subsequent send or NULL flush — flush before every reallocation.

### 2026-08-28 — Metal OFX render semantics (P — verified against Resolve 21's shipped GainPlugin sample, v1.4.0)
Facts the GPU fast path is built on, from `/Library/Application Support/Blackmagic Design/DaVinci Resolve/Developer/OpenFX/GainPlugin/MetalKernel.mm`:
- Declaring `kOfxImageEffectPropMetalRenderSupported = "true"` in describe makes Resolve set `kOfxImageEffectPropMetalEnabled` per render; when set, `kOfxImagePropData` on the clip images is an **`id<MTLBuffer>`** (cast the pointer), *not* CPU memory — the old `memcpy` passthrough would crash on it.
- The host's `id<MTLCommandQueue>` arrives via `kOfxImageEffectPropMetalCommandQueue`. **Encode onto that queue** — same-queue ordering is the only guarantee that the host's upstream renders finished writing the source buffer (our own queue would race). The sample commits without waiting; we wait only on command buffers whose output the CPU reads back (NDI conversion).
- Sample kernels index buffers tightly packed (`(y*width + x) * 4`), i.e. Resolve hands tight rows; the fast path still passes rowBytes through and the kernels honor it.
- Pipelines (and buffers) are **device-bound**, and the host queue's device can differ from `MTLCreateSystemDefaultDevice()` on multi-GPU Macs — the sample caches pipelines per queue; we cache per device.
- Metal shaders in this plugin compile from source **at runtime** — a shader typo builds fine in Tier 0 and dies inside Resolve. `make test-metal` compiles and runs the real kernels against the CPU reference; run it whenever `MetalGPUAcceleration.mm` changes.
**Rule:** under Metal render, never touch image data pointers with CPU code, and always encode on the host's command queue.

### 2026-08-28 — Leaked NDI sender advertisements lock the source name machine-wide (v1.4.0 verification session)
**Symptom:** after a stereo destroy-war session (the OPEN entry above), `NDIlib_send_create` for the default name failed on every render **even after switching the project to Vision: Mono** — the plugin logged "NDI runtime not being available" although `NDIlib_initialize` kept succeeding.
**Root cause:** verified with a standalone sender CLI linked against the same Advanced SDK dylib: the runtime was healthy (a fresh-name sender created and streamed 90 frames), but creating `DaVinci Resolve NDI Output` failed from *any* process. `dns-sd -B _ndi._tcp` showed that name still advertised from this machine by the still-running wounded Resolve process — senders orphaned when `NDIlib_destroy()` was called under them keep their Bonjour registration until the process exits, and NDI 6.2 `send_create` **fails outright on a same-machine name collision** (no auto-suffix).
**Fix:** v1.5.0 removes every `NDIlib_destroy()` call (the sender hub keeps the library for the process lifetime), so the plugin can no longer orphan its own senders. A name leaked by an *older* plugin build or another app still locks that name: change the NDI Source Name in the plugin UI (a fresh name registers immediately, no restart — and v1.5.0's Stream Status parameter now names this failure), or fully quit the offending process.
**Validated by:** scratch CLI (`NDI_DIAG_TEST` create + stream OK; default name create FAIL, exit 2) plus the dns-sd browse showing the orphaned advertisement while no working sender existed.
**Rule:** "Failed to create NDI sender" ≠ missing runtime — check `dns-sd -B _ndi._tcp local.` for a stale advertisement of that name first; and never call `NDIlib_destroy()` while senders are live, because it leaks their name registrations for the life of the process.

### 2026-08-28 — Stereo eye pairing shipped as a process-global hub (v1.5.0, issue #6)
**Symptom:** (feature work, plus the two stereo lifecycle bugs above) stereo timelines had no usable NDI output at all — the per-instance senders fought and the feed went black.
**Root cause:** everything instance-local: senders, the NDI library lifetime, and no pairing at all — while Resolve delivers stereo as per-eye render calls on two separate instances.
**Fix:** `src/StereoPair.h` (pure, host-free, unit-tested) decides per submitted frame: mono streams untouched; stereo latches on the first right-eye render and holds frames keyed on exact frame time over a bounded reorder window (cap 8, timeout 1 s); completed pairs pack Side-by-Side or Top-Bottom (left eye left/top) and go out as one frame on the shared sender; a partner eye silent >1.5 s degrades to labeled single-eye mono (Stream Status parameter + log), and the partner returning re-latches stereo. Thumbnails never touch pairer state and are dropped mid-stereo. A submit that doesn't send (Hold/Drop) NULL-flushes any in-flight async send so no instance's buffer is overwritten while NDI still reads it (the async-buffer rule above — with pairing, the next real send can be arbitrarily far away).
**Validated by:** `make test` (60+ assertions on the seam: reorder, duplicates, meta mismatch, eviction, timeout, starvation, recovery, packer byte layout); Tier 0; `make test-metal` unaffected. Tier 1–2 passed 2026-08-28 (Matt): stereo timeline streams, mono unchanged, all as expected.
**Rule:** when a host fans one logical output across multiple plugin instances, design the merge point process-globally FIRST — every instance-local resource it touches (senders, buffers, status) either moves into the shared hub or gets copied on the way in.

### 2026-08-28 — 8K playback collapse: NDI work must never block inside the render action (v1.6.0, issue #5)
**Symptom:** 8K stereo timeline (8160×7200, Resolution Half): 30 fps with the node bypassed, **5 fps** with NDI streaming on the GPU path, ~12 fps with the node active but NDI disabled, ~8–12 fps at Quarter.
**Root cause:** measured, not guessed — `make bench` (standalone harness at production dims) cleared the pipeline itself (sustains 30 pairs/s; SpeedHQ encode ceiling 153 fps; the per-pair NULL flush waits ~0; clock_video pacing fine), while the in-host `mtimer` per-stage timers showed each render action blocking ~90 ms (≈20 ms `waitUntilCompleted` + ≈65 ms size-dependent residual in the conv→submit path) — and Resolve renders **both eyes serially on one thread**, so ~190 ms of every 200 ms pair period was spent inside our render actions. Separately: with ANY active OFX Metal node on this timeline, Resolve itself caps playback at ~12 fps (blit-only measurement) — a host ceiling no plugin change can move.
**Fix:** v1.6.0 non-blocking fast path. The fused kernel writes directly into CPU-visible shared staging slots (ring of 4, `metal_gpu_downscale_submit` — on unified memory the small converted frame needs no separate readback); the render action encodes and returns; Metal's completion callback (microseconds) wakes a per-instance pump worker that pairs, packs, and sends off-thread. Worker sends are synchronous, so no NDI in-flight buffer ever aliases a staging slot. Backpressure anywhere (slot ring, pump queue) drops the frame — a preview stream must never block the host.
**Validated by:** `make test` (117 asserts), `make test-metal` (13, incl. async ring: refuse-when-full, released-slots-reusable, output bit-equal to blocking path), `make bench` stage E3 (enqueue never blocks; worker sends at clock rate; clean drops). Tier 1–2: PENDING (this entry updates when it passes).
**v1.6.1 follow-up — the wait MOVED instead of dying (Tier 2 on v1.6.0, 2026-08-28):** playback stayed ~5–6 fps although the render action's GPU wait was gone (enqueue 0.0 ms med) — mtimer totals (~70 ms) now matched the concurrent wtimer totals almost line-for-line: the render thread was queueing on `hub->mutex` in `ensureNDIReady` while a pump worker held that lock through a ~30 ms cold-page pairer copy plus a ~15 ms sync send (two per-instance workers + render threads, all serialized on one mutex). Fix: (a) lock-free `senderReady` atomic answers the render-path's only question without the mutex; (b) the pairer recycles hold-payload buffers (`kPayloadPoolCap`) so the hold is a warm-page memcpy, and the hub returns consumed mate buffers via `pairer.recycle()`.
**Rules:** (1) inside a host render action, never wait on the GPU and never run CPU-heavy or SDK work — encode, hand off, return; anything slower than ~1 ms belongs on a worker with drop-based backpressure. (2) Before optimizing a slow in-host path, benchmark the same call pattern standalone (`make bench`) — it exonerated four plausible bottlenecks in one run. (3) A per-frame cost that vanishes when the feature is off but isn't in your stage timers is still YOUR cost — widen the timers until the residual is zero or move the whole path off-thread. (4) Moving work off the render thread is not enough if the render path still takes a lock the worker holds — audit every mutex the hot path touches for worst-case hold time, and answer hot-path questions with atomics. (5) A multi-MB `std::vector` allocated per frame is a page-fault storm, not a memcpy — pool and recycle frame-sized buffers.

### OPEN — Windows/CUDA build failing (as of 2026-08-28)
Commit `50eacc1` added the CMake + CUDA port ([CMakeLists.txt](CMakeLists.txt), [src/CudaGPUAcceleration.cu](src/CudaGPUAcceleration.cu), two build .bat variants) but it has not yet produced a working build. Needs a Windows machine with VS 2019+/CUDA 11+/NDI 6 Advanced SDK to iterate. Record the actual failure output here when work resumes — "failing" without the error text is unactionable.

---

## Template for new entries

```markdown
### YYYY-MM-DD — <short title>
**Symptom:** what was observed.
**Root cause:** what was actually wrong.
**Fix:** what changed (commit/PR link).
**Validated by:** which tier of the testing loop / what evidence.
**Rule:** one sentence that prevents recurrence.
```
