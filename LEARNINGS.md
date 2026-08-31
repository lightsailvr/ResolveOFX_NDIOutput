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
**Validated by:** `make test` (117 asserts), `make test-metal` (13, incl. async ring: refuse-when-full, released-slots-reusable, output bit-equal to blocking path), `make bench` stage E3 (enqueue never blocks; worker sends at clock rate; clean drops). **Tier 1–2 passed 2026-08-28 (Matt, v1.6.1):** render-action blocking measured 0.00 ms med / 0.10 ms p90 / 0.3 ms max (n=792, mtimer), GPU exec 0.57 ms med, 770/770 renders GPU-native async, 0 CPU fallbacks, 16 frames dropped by backpressure; playback 10–12 fps and "much more responsive" (note: that final run had Timeline Proxy at half — renders arrived 4080×3600/eye; the full-res 8160×7200 ceiling with any active OFX Metal node remains ~8–15 fps per the blit-only A/B, and the render-action numbers are dimension-independent).
**v1.6.1 follow-up — the wait MOVED instead of dying (Tier 2 on v1.6.0, 2026-08-28):** playback stayed ~5–6 fps although the render action's GPU wait was gone (enqueue 0.0 ms med) — mtimer totals (~70 ms) now matched the concurrent wtimer totals almost line-for-line: the render thread was queueing on `hub->mutex` in `ensureNDIReady` while a pump worker held that lock through a ~30 ms cold-page pairer copy plus a ~15 ms sync send (two per-instance workers + render threads, all serialized on one mutex). Fix: (a) lock-free `senderReady` atomic answers the render-path's only question without the mutex; (b) the pairer recycles hold-payload buffers (`kPayloadPoolCap`) so the hold is a warm-page memcpy, and the hub returns consumed mate buffers via `pairer.recycle()`.
**Rules:** (1) inside a host render action, never wait on the GPU and never run CPU-heavy or SDK work — encode, hand off, return; anything slower than ~1 ms belongs on a worker with drop-based backpressure. (2) Before optimizing a slow in-host path, benchmark the same call pattern standalone (`make bench`) — it exonerated four plausible bottlenecks in one run. (3) A per-frame cost that vanishes when the feature is off but isn't in your stage timers is still YOUR cost — widen the timers until the residual is zero or move the whole path off-thread. (4) Moving work off the render thread is not enough if the render path still takes a lock the worker holds — audit every mutex the hot path touches for worst-case hold time, and answer hot-path questions with atomics. (5) A multi-MB `std::vector` allocated per frame is a page-fault storm, not a memcpy — pool and recycle frame-sized buffers.

### 2026-08-30 — Projection normalization shipped: sender-side STMap fisheye→equirect warp (v1.7.0, issue #7)
**Symptom:** (feature work) an Apple Immersive fisheye timeline viewed in the Quest stereo-180 player is geometrically wrong — receivers assume equirect.
**Fix:** Projection parameter (Passthrough / Equirect (STMap)) with per-eye EXR STMap file params. Design decisions worth remembering:
- **The map defines the destination image:** warped output dims = the STMap's dims (then the Resolution divisor applies) — so warp+downscale compose as one GPU gather with **no intermediate full-res buffer** (an 8K float RGBA intermediate would be ~940 MB/frame). The fused warp kernels run through the same v1.6.0 non-blocking slot-ring; the render action still only encodes.
- **Every failure is soft** (acceptance criterion): missing/invalid/truncated/hostile EXR, PIZ/tiled/deep/multipart, GPU map-upload failure, L/R map size mismatch — all stream passthrough plus a Stream Status message, never a crash or a black feed. L/R maps disagreeing on size must NOT half-engage: different per-eye frame sizes are unpairable, so mismatch = passthrough.
- **Maps are process-shared** like the sender hub (weak-ptr cache keyed on path, reload on mtime/size change): both stereo eye instances name the same files, and a big map is hundreds of MB. Loads run outside every lock; render threads swap in the result via shared_ptr copy; replaced maps are released outside the swap mutex; and the per-device GPU upload is pre-warmed at refresh time on the main thread (multi-GPU Macs upload once on the render path, also outside the entry mutex) — all four points direct applications of the v1.6.1 mutex/hot-path rules.
- **Passthrough is bit-identical to v1.6.1** structurally: warp code only runs behind `renderStmap != null`; the plain kernels and CPU paths are untouched.
- **Orientation conventions (T until Tier 3):** EXR rows are stored top-down; STMap R/G = normalized source U/V with bottom-left origin (Fusion/Nuke convention); OFX frames are bottom-up. Unit tests pin the composition (identity map ≡ plain downscale byte-for-byte, on CPU and GPU), but the conventions against a REAL Fusion-authored map need the headset check — if the image lands flipped, the fix is one sign in `ndi_stmap::warpRGBABox` + the `warp_sample_rgb` kernel, both under test.
- Resolve's rendering of `kOfxParamStringIsFilePath` params (browse button vs plain text field) is (T) — typing a path works regardless.
**Validated by:** `make test` (158 asserts; 41 new: EXR reader vs third-party ffmpeg-written fixtures AND in-test-built files incl. hostile inputs; CPU warp identities) + `make test-metal` (25: identity-map warp ≡ downscale kernels byte-for-byte, warp vs CPU reference, async ≡ blocking) + Tier 0. Tier 1–3 pending (needs a fisheye timeline, a Fusion STMap, and the Quest player).
**Rule:** validate a hand-rolled binary-format reader against files written by a third-party implementation, never only against your own test writer — a shared misunderstanding of the format reads as green (here: ffmpeg-encoded EXR fixtures caught nothing *because* the predictor/interleave order was right, but only they could have).

### 2026-08-30 — Packed side-by-side STMap layout (v1.8.0, Canon VR workflows)
**Symptom:** (feature request, Matt) Canon VR workflows author ONE side-by-side packed STMap, not two per-eye files; wanted it usable directly.
**Fix:** `STMap Layout` param (Per-Eye Files / Packed Side-by-Side). Packed layout splits the single EXR into per-eye maps (`ndi_stmap::splitPackedSTMap`), auto-detecting each half's U convention from its valid texels — a real per-eye fisheye→equirect map spans nearly the full [0,1] U range while a packed-frame half sits inside half of it (thresholds 0.45/0.55) — and rescaling packed-frame U to per-eye. Per-half detection also absorbs maps that bake in the Canon eye swap. Split halves cache process-wide under pseudo-keys (`path + "\n#packedSbS:L/R"` — '\n' can't occur in a real path); the packed image frees once both halves exist. The decision is logged per half (`STMap packed SbS split: ...`) — check it when a packed map looks wrong.
**Key insight:** a packed-frame source on a MONO timeline needs no splitting at all — Per-Eye Files layout with the packed map in the left slot warps the whole frame in one pass (worked since v1.7.0); the split only exists for Stereo 3D timelines where Resolve renders each eye separately, and there the plugin structurally cannot reproduce a cross-eye swap (each render only has its own eye's pixels) — eye assignment is the timeline's job.
**Validated by:** `make test` (179 asserts; 21 new incl. packed-identity ≡ two per-eye identities, the swapped-halves case, invalid-texel preservation); Tier 0. Tier 1–3 pending with the rest of the projection work. Also flipped the STMap path params to the spec-default `FilePathExists=1` so any host-rendered picker is an open-existing dialog (whether Resolve draws one at all is still (T), Tier 1).
**Rule:** when a format has two plausible coordinate conventions in the wild, don't add a mode switch the user must get right — classify from the data when the conventions are unambiguous there, and log the decision so a wrong guess is diagnosable.

### 2026-08-30 — Canon STMap's zero-filler defeated the packed-half auto-detect (v1.8.1)
**Symptom:** first real Tier-2 run of the packed SbS layout (Canon EOS R5C RF5.2mm map, stereo timeline): the NDI stream's left eye showed a big black disc; right eye fine. Stream Status healthy ("Stereo (Side-by-Side), Equirect (STMap)") — the pipeline ran, the geometry was wrong.
**Root cause:** diagnosed from data, not the screenshot — the plugin's own split log line (`left half: per-eye coords (copied); right half: packed-frame coords`) plus a scratch analyzer over the real EXR through the production loader. The map's left-eye half samples U∈[0.5,1] of the packed frame (Canon's eye swap) with **~1% of texels at (0,0)** — Canon pads content-free corners with zeros, not out-of-range values. Auto-detect classified on absolute uMin/uMax, so uMin=0 pushed the half to "per-eye, no rescale" → the un-rescaled map sampled only half of the eye's frame → black disc. (L)
**Fix:** classification keys on the mass of the valid-U distribution — a half is packed-frame when ≤5% of its valid texels sit on the wrong side of the 0.45/0.55 marks (`kPackedDetectMaxSpill`). Filler rescales out of range → black, correct for content-free regions. Regression test builds the filler pattern; a differential harness (mono full-frame warp vs split per-eye warp, black-fraction metric) went red at 26.3% left-eye black on the real map and green at 3.6% after.
**Validated by:** `make test` (182 asserts), `make test-metal`, Tier 0; real-map differential harness green. Tier 2 re-check pending (also watch: the log showed the stream flapping Stereo↔"left eye missing" every few seconds — consistent with parked-timeline phantom renders per the pitfalls table; confirm it disappears during actual playback before treating it as a bug).
**Rule:** classifiers over real-world image data must key on the distribution's mass, never its extremes — production files carry filler/sentinel texels, and one outlier must not flip a decision; and when a warp misbehaves, read the plugin's own decision log lines before theorizing from pixels.

### 2026-08-30 — Resolve draws no browse control on OFX filePath params; shipped native open-panel buttons (v1.9.0)
**Symptom:** the STMap path params declared `kOfxParamStringIsFilePath` (the only file-dialog mechanism OFX offers a plugin), but Resolve 20/21 renders them as plain text fields — no browse button (L, 2026-08-30, Matt's v1.8.0 session; resolves the (T) from the v1.7.0 entry).
**Fix:** macOS-only `kOfxParamTypePushButton` params ("Browse for Left/Right-Eye STMap…") whose instanceChanged handler pops an `NSOpenPanel` (`src/MacFileDialog.mm`, links AppKit + UniformTypeIdentifiers) and writes the picked path into the string param, then falls through to the normal param-read + map-refresh path. Guards: main-thread-only (a modal nested event loop off-main is a deadlock risk — the dialog just returns false there), cancel changes nothing, paste-in still works.
**Validated by:** Tier 0 + `make test`/`make test-metal` (unchanged). (T): the modal `runModal` spins a nested event loop inside Resolve's parameter-change action — verify at the next Tier 1 that clicking Browse opens the panel and Resolve stays stable afterward; if the host ever misbehaves, the buttons are trivially removable and the text fields carry the feature alone.
**Rule:** when a host doesn't implement an OFX affordance, the plugin can often supply it natively — but anything that runs a nested event loop inside a host callback ships with a fail-soft path and gets flagged for in-host verification.

### 2026-08-30 — BRAW SDK's factory functions aren't linkable symbols; resolve the framework from the host bundle (v1.10.0)
**Symptom:** first standalone build against `BlackmagicRawAPI.framework` failed at link — `Undefined symbols: _CreateBlackmagicRawFactoryInstanceFromPath` — despite `-framework BlackmagicRawAPI` and the header declaring it `extern "C"`.
**Root cause:** the BRAW SDK never intends direct linking. The factory entry points live only behind `BlackmagicRawAPIDispatch.cpp` (shipped in the SDK's Include dir), which locates and `CFBundle`-loads the framework at runtime — main bundle's `Contents/Frameworks` first, then exe-relative, then an explicit path. That "bundle first" order is a gift for an OFX plugin: inside Resolve it binds **Resolve's own shipped framework** (identical 5.1 build to the SDK's), so the plugin links nothing, ships nothing, and can't version-skew against the host.
**Fix:** vendored the header + shim (Boost-style license) into `third_party/braw/`; `src/BRAWImmersiveReader.cpp` compiles the shim into its one TU and falls back to the SDK install path for host-less binaries. Feature: Equirect (Camera Metadata) — per-eye warp maps generated from the URSA Cine Immersive embedded calibration (#11), design + verification in `docs/2026-08-30-braw-immersive-metadata-projection.md`.
**Validated by:** Tier 0 + `make test` (incl. new `test_brawmap` goldens vs an independently verified implementation) + `make test-metal`; scratch end-to-end harness: real .braw → reader → generated maps → production `warpRGBABox` → correct equirect (masked and unmasked) on a decoded frame. Tier 1–2 pending (needs Resolve restart + human playback).
**Rule:** when an SDK ships a "dispatch" TU next to its header, that's the supported loading mechanism — use it rather than fighting the linker, and inside a host process prefer resolving the host's own copy of a shared runtime over shipping your own.

### 2026-08-30 — Resolve's scripting API is reachable from inside the plugin via a spawned helper; auto camera-clip mode (v1.11.0)
**Symptom:** Camera Metadata mode's manual clip pick worked at Tier 2 (U — Matt: "with the camera clip selected, this works fine"), but manual selection can't serve multi-camera timelines — the calibration must follow the clip under the playhead.
**Root cause / finding:** the scripting API isn't linkable from C++ — `fusionscript.dylib` is a Python extension — but a child process spawned FROM INSIDE Resolve can use the standard `DaVinciResolveScript` route and query `GetCurrentTimeline().GetCurrentVideoItem().GetMediaPoolItem().GetClipProperty("File Path")` against its parent. Verified live: macOS system python3 (3.9.6) loads the bundled fusionscript cleanly and returns the playhead clip's path in ~ms, with the external-scripting preference (Local) enabled.
**Fix:** `src/ndi_timeline_watch.py` (bundled in Contents/Resources) polls at 2 Hz and prints path-per-line; `src/TimelineClipWatcher.cpp` owns the helper (posix_spawn, reader thread, 30 s respawn backoff, SIGTERM+EOF shutdown — only the reader thread ever closes the pipe fd, a cross-thread close could double-close a host-reused fd) and fans deduped changes out to registered instances, which re-source their lens maps (per-camera cached, so cuts between known cameras swap in µs). Sticky rule: gaps/non-BRAW/uncalibrated clips under the playhead KEEP the last camera's maps — a monitoring stream must never pop its geometry at a cut. New "Camera Clip Source" param: Timeline (Auto, default) / Manual Path (#11).
**Validated by:** Tier 0 + `make test`/`make test-metal`; standalone watcher harness against the live Resolve session (correct path within 0.5 s, healthy status, clean shutdown, no leaked helper). Tier 1–2 pass (U, Matt, 2026-08-30): auto mode confirmed working in-session ("this feature is implemented"). Still worth a look when convenient: auto-follow across an actual multi-camera timeline, and the scripting-disabled fallback path. Same session surfaced the stereo↔mono flapping while PARKED on a frame — filed as #12 (canvas-stability + hysteresis guard) with the parked-timeline phantom-render pattern as prime suspect.
**Rule:** when a host exposes functionality only through a scripting runtime, a small spawned helper speaking that runtime beats linking gymnastics — keep it out-of-process, heartbeat its output, and make every failure fall back to the manual path.

### 2026-08-30 — Stereo↔mono canvas pop on eye stalls; locked canvas + hysteresis (v1.12.0, #12)
**Symptom:** (U, Matt, v1.10.0 session) the NDI stream flips stereo SbS → mono and back noticeably often, worst while parked on a frame — in-headset the geometry pop is a severe nausea trigger.
**Root cause:** the v1.5.0 pairer treated 1.5 s of partner-eye silence as starvation and degraded to true single-eye mono — the outgoing frame shrank from 2W×H to W×H, so receivers re-fit (the pop); the first returning frame then re-latched stereo immediately. Parked-timeline phantom renders (pitfalls table) arrive sporadically and often single-eyed, so those two hair triggers oscillated the mode endlessly while parked.
**Fix:** canvas lock + two-sided hysteresis in the pairer ([src/StereoPair.h](src/StereoPair.h)): once stereo latches, no single-eye frame is ever emitted again — degraded mode packs the flowing eye into BOTH halves (new `SendDuplicate` action) on the unchanged canvas, and thumbnails now `Drop` while degraded too (a latent pop in the old fallback). Degrading needs sustained partner silence (`kDegradeSilenceMs` = 4 s, clock restarted when the submitting eye resumes from its own long stall — mutual UI stalls don't count) *while the flowing eye advances through ≥2 distinct frame times* — same-time phantom re-holds never count, so a parked timeline can no longer degrade at all. Re-latching needs a sustained run from the missing eye (3 distinct-time, meta-matching frames, gaps <1.5 s). During a stall, holds past 700 ms re-send the last packed frame (new `RepeatLast` action) so receivers keep getting frames through the longer window.
**Validated by:** `make test` (117 stereo-pair asserts: hysteresis transitions both directions, parked no-flap, mutual-stall resume, keepalive gating, and an acceptance sweep asserting no single-eye send ever occurs after latch), `make test-metal`, Tier 0. Tier 1–2 pass (U, Matt, 2026-08-30, in-headset): parked stereo holds with no flap; a forced stall (Vision→Mono during playback) goes flat on the same canvas after ~4 s with no pop; flipping back restores depth near-instantly. Log capture during the session (`monitor_ndi_logs.sh`): the only degrade/recover cycles were the two forced flips (degrade 4.1–4.8 s after single-eye renders resumed, recovery ≤2 s after the eye returned, pairer drops +2 across both) and real playback showed no spontaneous flaps — the "why more frequent in v1.10.0" question closes with the guard rather than an answer (no starvation left to capture). Gotcha for future stall tests: changing the Stereo 3D palette mid-play halts Resolve's transport (host behavior), and parked content then streams as a static frame BY DESIGN — degraded-mode motion only shows after pressing play again.
**Rule:** a monitoring stream's geometry is API — never let a health heuristic change outgoing frame dimensions; degrade inside the canvas already advertised, and damp both directions of any automatic mode flip so a flapping input can't flap the output.

### 2026-08-30 — Every binary shipped so far was stamped minos = build-host OS (26.0)
**Symptom:** none observed locally — found by inspection while building the release pipeline: `otool -l NDIOutput.ofx | grep minos` showed `26.0`, meaning the plugin would be *refused by dyld* on any macOS older than the build machine's, with a cryptic host-side load failure.
**Root cause:** the Makefile passed no `-mmacosx-version-min`, and clang then stamps `LC_BUILD_VERSION.minos` with the host OS version. Every dev machine upgrade silently raised the floor for anyone we handed a build to.
**Fix:** `DEPLOYMENT_TARGET ?= 13.0` wired into CXX/OBJCXX/LD flags (13.0 = `libndi_advanced.dylib`'s own minos, so nothing lower is reachable anyway). Release packaging also builds universal via `ARCHFLAGS="-arch arm64 -arch x86_64"` — compiles clean, no arch-specific code in src/.
**Validated by:** `otool -l` shows minos 13.0 on both slices; Tier 0 + `make test` + `make test-metal` pass on the universal build.
**Rule:** any binary that might leave the build machine needs an explicit deployment target; verify with `otool -l <bin> | grep -A2 LC_BUILD_VERSION` before handing a build to anyone.

### 2026-08-30 — codesign is a bundle-layout linter: three packaging gotchas (release pipeline, v1.13.0)
**Symptom:** first `codesign --verify --strict` of the staged bundle failed twice: "code object is not signed at all / In subcomponent: …Contents/BaldavengerOFX.NDIOutput.png", then "a sealed resource is missing or invalid / file modified: …Contents/macOS/NDIOutput.ofx".
**Root cause:** (1) the icon PNG sat at `Contents/` top level (Baldavenger-template layout) — unknown top-level files aren't part of the resource seal and fail strict verification; OFX's documented icon location is `Contents/Resources/` anyway (and the name `BaldavengerOFX.NDIOutput.png` can never match our `LSVR.NDIOutput` identifier, so it's inert either way). (2) The executable dir was spelled `Contents/macOS` — codesign's seal walker only excludes the canonical `MacOS` spelling, so the main binary ALSO got hashed as a sealed resource, then "modified" when its own signature was embedded. Case-insensitive APFS hid this for the project's whole life because dyld/Resolve load it fine either way. (3) Separately, `pkgbuild --root` defaults to relocatable bundles — the installer would "upgrade" whatever copy of the bundle Spotlight indexed (i.e. the dev checkout in this repo!) instead of `/Library/OFX/Plugins`.
**Fix:** Makefile now creates `Contents/MacOS/` and puts the PNG in `Contents/Resources/`; `scripts/package_release.sh` sets `BundleIsRelocatable=false` via `pkgbuild --analyze` + PlistBuddy (`Set` falls back to `Add` — the key isn't always emitted). Verified: bundle signs and passes `--strict --deep`, pkg PackageInfo shows `install-location="/Library/OFX/Plugins" relocatable="false"`, and the signed bundle dlopens with `OfxGetPlugin` resolvable and the bundled `@loader_path` dylib loading.
**Validated by:** Tier 0 + full `--unsigned-dev-build` pipeline run; Tier 1–2 pass (U, Matt, 2026-08-30) with the pkg-installed release build — plugin loads and reports v1.13.0 with the bundled `@loader_path` dylib.
**Rule:** run `codesign --verify --strict` against the bundle long before release day — it is the only tool that checks bundle layout, and layout mistakes are invisible until signing.

### 2026-08-30 — Installer "succeeded" but silently skipped the plugin: stale Info.plist "2.0" outranked the 1.13.0 pkg
**Symptom:** (U, Matt, first pkg Tier 1) after installing the signed `NDIOutput-1.13.0-macOS.pkg`, Resolve still showed v1.12.0. The pkg receipt (`pkgutil --pkg-info`) claimed 1.13.0 was installed.
**Root cause:** `pkgbuild --analyze` defaults `BundleIsVersionChecked=true`, and the repo's `Info.plist` had carried a placeholder `CFBundleShortVersionString` of **2.0** for the project's whole life — every dev `make install` stamped that into `/Library/OFX/Plugins`. PackageKit compared installed (2.0) vs payload (1.13.0), decided the pkg was a downgrade, and skipped the component — `install.log`: *"Skipping component LSVR.NDIOutput (1.13.0-…) because the version 2.0.0-… is already installed"* — **while still writing the receipt**, so the install looked successful everywhere except the actual plugin.
**Fix:** `package_release.sh` now sets `BundleIsVersionChecked=false` (a plugin installer must replace unconditionally — `BundleOverwriteAction=upgrade` then swaps the whole bundle atomically); repo `Info.plist` set to the real version; both version scripts (`set_version.sh`, `increment_version.sh`) now sync `Info.plist` as a third location so dev installs can never poison the comparison again.
**Validated by:** the rebuilt pkg's PackageInfo shows an empty `<bundle-version/>` (vs the first build enrolling the bundle for version checking); regression case exercised for real — re-install over the untouched stale "2.0" bundle replaced it, and Resolve shows v1.13.0 (U, Matt, 2026-08-30).
**Rule:** for plugin/bundle pkgs, always disable PackageKit's bundle version check, and after any pkg install trust `grep <bundle-id> /var/log/install.log` over the receipt — receipts get written even for skipped payloads.

### 2026-08-30 — Resolve honors runtime kOfxParamPropSecret: mode-aware inspector panels work
**Symptom:** (capability verification, not a bug) the projection group stacked every mode's map-source fields at once — per-eye + packed STMap slots and the camera-clip picker — regardless of which mode could read them. Unknown whether Resolve only reads `kOfxParamPropSecret` at describe time or honors runtime edits.
**Root cause:** n/a — v1.14.0 UI cleanup.
**Fix:** `updateParamVisibility()` flips `kOfxParamPropSecret` per param via `paramGetPropertySet` + `propSetInt`, called at createInstance (restores a saved project's visibility) and on every instanceChanged; describe-time secret states match the param defaults so a fresh instance is right before the first sync.
**Validated by:** Tier 1–2 pass (Matt, 2026-08-30): fields swap live in the inspector when STMap Layout / Camera Clip Source change, with no reselect needed, and saved projects restore the correct set.
**Rule:** Resolve repaints on runtime `kOfxParamPropSecret` edits to instance params — hide inapplicable params instead of stacking every mode's fields.

### 2026-08-30 — CLOSED: the May-2025 Windows/CUDA scaffold was deleted, not diagnosed (ticket #20)
The `50eacc1` scaffold (host-memory CUDA sketch, D3D11 "fallback" that converted nothing, MinGW .bat that could never work — nvcc requires MSVC as host compiler) predated the plugin's modern architecture and was **deleted per spec decision 7** (docs/windows-port-spec.md); git history keeps it. The Windows build restarted on good bones on branch `windows-port`: CPU-only compile + the portable unit suite are green in CI on every push (`.github/workflows/windows.yml`), so the build can no longer rot unobserved. CUDA returns properly with ticket #22.

### 2026-08-30 — `1L << 31` is negative on Windows: STMap size cap rejected every EXR
**Symptom:** (CI, first MSVC run of the portable suite) all `test_stmap` file-based cases failed with the reader's "cannot read … (empty, unreadable, or over 2 GiB)" soft-fail — for 5 KB fixture files that macOS read fine.
**Root cause:** `loadSTMapEXR` capped file size with `fileSize <= (1L << 31)`. `long` is 32-bit on Windows (LLP64), so `1L << 31` evaluates to a negative value and the guard rejected every file. macOS (LP64, 64-bit `long`) never saw it.
**Fix:** `1LL << 31` (src/STMap.h; branch `feature/win-ci-compile`, ticket #20).
**Validated by:** Windows CI — `test_stmap` 100% green under MSVC; macOS `make test` unchanged.
**Rule:** never assume `long` is 64-bit — on Windows it is 32; use `long long`/`int64_t` (and `1LL` shifts) for any byte-size or offset math that must survive both platforms.

### 2026-08-30 — CI can link against the access-gated NDI SDK via vendored MIT headers + a stub import library
**Symptom:** (ticket #20 design) hosted CI must compile *and link* the plugin, but the NDI **Advanced** SDK download is access-gated and its import library is not redistributable — no SDK can live on the runner.
**Root cause:** n/a — distribution constraint, not a bug.
**Fix:** the SDK's `Processing.NDI.*.h` headers each carry their own per-file MIT license ("applies to this file ONLY"), so the needed 13 are vendored under `third_party/ndi/include/`; the DLL's export-name list (extracted by `scripts/dump_ndi_exports.py`, a ~50-line PE parser) is checked in as a `.def`, and CMake generates one empty C function per export to build a stub DLL whose import library the linker consumes. The stub ships nowhere and never loads; builds on a machine with the real SDK use it automatically. Two sub-gotchas: the generated stub is C, so `project()` must enable `C` alongside `CXX` (CMake dies with "cannot determine linker language" otherwise), and the import must be `/DELAYLOAD`ed anyway per spec decision 12.
**Validated by:** Windows CI green: full plugin compile + link with zero NDI bits on the runner.
**Rule:** headers' per-file licenses can differ from their SDK's — read them before assuming a gated SDK blocks CI; a `.def`-generated stub import library is all MSVC needs to prove a link.

### 2026-08-30 — First MSVC pass over "portable" code: the small fix list
**Symptom:** (CI, ticket #20) code that built clean under clang for years needed four kinds of touch-up under MSVC.
**Root cause:** libc++ transitively includes what MSVC's STL doesn't (`<cmath>` via `<algorithm>`); POSIX `mkdir`/`0755` doesn't exist (`_mkdir` in `<direct.h>`); `M_PI` needs `_USE_MATH_DEFINES`; UTF-8 string literals (log emoji) need `/utf-8` or MSVC decodes source bytes through the ANSI code page.
**Fix:** explicit includes + a `_mkdir` shim in the one test that makes a directory; `/utf-8 _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS` centralized in CMakeLists.txt.
**Validated by:** Windows CI — plugin + all 6 portable tests compile and pass under MSVC; macOS `make test` unchanged.
**Rule:** include what you use (never trust transitive STL includes), and treat `/utf-8` as mandatory for any MSVC target whose sources hold non-ASCII literals.

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
