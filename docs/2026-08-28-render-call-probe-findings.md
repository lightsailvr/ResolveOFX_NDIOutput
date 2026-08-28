# Render-Call Probe Findings: What Resolve Feeds the OFX Render Action

**Date started:** 2026-08-28 · **Plugin:** NDIOutput ≥ v1.3.0 · **Host:** DaVinci Resolve Studio 21.0.4, macOS 26.x
**Issue:** [#3](https://github.com/lightsailvr/ResolveOFX_NDIOutput/issues/3) — instrument every render call (eye, page, thumbnail, cadence) so host behavior is observed, not guessed.

**Status: COMPLETE (2026-08-28).** Captured: a VR180 project (4096×4096/eye) under both Stereo 3D palette Vision modes on Edit and Color pages, and the true Apple Immersive pipeline (8160×7200/eye). Descoped by Matt: scenario 1 (mono packed passthrough — proven by daily use) and scenario 4 (Timeline Proxy Mode — replaced by the product requirement that the *plugin* offer stream downscaling, Nobe-style → issue #5). Headline: with **Vision: Stereo, both eyes render every frame — each through its own plugin instance** (keyed by `time`/`src`; arrival order NOT guaranteed); at Vision: Mono the right eye never renders. Side-discovery: the current plugin's NDI output goes **black** in stereo mode (sender lifecycle bug, feeds issue #6).

**Correction (Matt, 2026-08-28):** the timeline used for the 4096×4096 captures below was a **VR180 project**, not a plain stereoscopic timeline — so those captures answer scenario 3's VR180 question directly, and the plain-stereo-timeline case is subsumed (same stereo machinery, per-eye mechanism confirmed across two immersive project types). Section headers below keep their original scenario numbering with corrected labels.

---

## 1. How to capture

1. Install v1.3.0+ (`sudo make install`, restart Resolve).
2. In the plugin UI, open the **Diagnostics** group and enable **Log Render Calls**.
3. In a terminal: `./scripts/capture_probe_log.sh <scenario-slug>` — it tees every probe line into `docs/captures/`.
4. Play back per the scenario, Ctrl-C the capture, commit the capture file, write up the findings below.

Unless a scenario says otherwise: **Playback → Render Cache → None**, background caching off, **Timeline Proxy Mode off**. Cache/proxy silently change what the plugin sees ([LEARNINGS.md](../LEARNINGS.md) §2, "behaviors that will lie to you").

## 2. Reading a probe line

```
NDI Plugin: probe #000042 page=Edit eye=R time=86400 src=86399 dim=1920x1080 scale=1.00x1.00 thumb=0 dt=16.7ms
```

| Field | Source | Meaning |
|---|---|---|
| `#000042` | plugin counter | Render-call sequence number, per effect instance (resets when the instance is recreated) |
| `page=` | `OfxImageEffectPropResolvePage` at createInstance | Page that instantiated the effect: `Edit`, `Color`, `Fusion`; `?` = host didn't provide it |
| `eye=` | `OfxImageEffectPropEyeToRender` per render | `L` (mono or left), `R` (right), `-` = property absent from this call, any other integer = raw undocumented value |
| `time=` | `kOfxPropTime` | Timeline frame being rendered (fractional if retimed) |
| `src=` | `OfxImageEffectPropSrcFrame` | Source frame the host reports; `-` = absent. Candidate key for pairing L/R eyes |
| `dim=` | render window | Width×height actually delivered — watch this in proxy mode |
| `scale=` | `kOfxImageEffectPropRenderScale` | Host render scale; whether proxy mode shows up here or only in `dim` is exactly what scenario 4 answers |
| `thumb=` | `kOfxImageClipPropThumbnail` on the source clip | `1` = thumbnail render (filmstrip/gallery), `-` = property absent |
| `dt=` | wall clock | Spacing since the previous render call on this instance; `-` = first call. Cadence questions are answered from this column |

One caveat for captures: the plugin declares fully-thread-safe rendering, so if Resolve ever renders concurrently, `dt` measures probe-lock spacing and lines can reach the log slightly out of `#` order — sort by `#` before doing cadence math on such a capture.

Answering cadence questions directly from a capture:

- **Playback rate:** steady `dt` ≈ 1000/fps (16.7 ms @ 60, 41.7 ms @ 24). Gaps = dropped/cached frames; `dt` ≪ frame period = burst rendering (e.g. both eyes back-to-back).
- **Second-eye behavior:** grep `eye=R`. Pairs look like `eye=L time=N` immediately followed by `eye=R time=N` (same `time`, tiny `dt`).
- **Thumbnail pollution:** `thumb=1` lines (also typically small `dim=`). These must be excluded from any cadence math.
- **Cached-frame starvation:** playback running but no probe lines at all — Resolve stopped calling render; that's the render cache, not a plugin bug.

## 3. Scenarios

### Scenario 1 — mono timeline, packed side-by-side media, Edit page

- **Slug:** `mono-packed-sbs-edit`
- **Setup:** mono (non-stereoscopic) timeline with pre-packed SbS media (e.g. 4320×2160, two 2160×2160 eyes). NDIOutput on an **adjustment clip on the Edit page**. Play ~15 s.
- **Questions:** Does every playback frame reach the plugin at full packed resolution? Is `dt` steady at the timeline frame period? Is `eye` absent or always `L` on a mono timeline? Does `page=Edit` come through (i.e. does Resolve actually provide the page property at createInstance)?

**Findings: DESCOPED (Matt, 2026-08-28).** The packed-media passthrough is the plugin's shipping behavior, proven by daily use; and the stereo detector in issue #6 should key on "did an R call arrive for this frame", not on the eye property's presence, so the one open nuance here (does a mono timeline report `eye=L` or omit the property?) decides nothing downstream. Capture it opportunistically if a packed-media project happens to be open with the probe on.

### Scenario 2 — stereoscopic timeline (captured on a VR180 project, 4096×4096/eye), Edit page vs Color page ⭐ the key unknown

- **Slugs:** `stereo-timeline-edit-page`, `stereo-timeline-color-page`
- **Setup:** native stereo timeline (stereo clips via *File → New Stereo Clip* or stereo-camera media). Two captures:
  1. NDIOutput on an Edit-page adjustment clip; play ~15 s on the **Edit page**.
  2. NDIOutput as a Color-page node; play ~15 s on the **Color page**. For a variant, arm the Stereo 3D palette Out mode (Side by Side) and re-capture.
- **Questions:** **Does the right eye render at all during Edit-page playback — and at what cadence?** (Every frame? Only the displayed eye? Only on pause?) Same for the Color page. Do `eye=L`/`eye=R` calls for one frame share `time`/`src` (→ pairing key for the stereo tap)? What order and spacing?
- **Why it matters:** architecture A in the [feasibility study](2026-08-28-resolve-stereo-program-tap-feasibility.md) §4 stands on pairing per-eye render calls; if the second eye never renders during Edit-page playback, the Edit-page stereo tap needs the palette armed, or falls back to architecture B.

**Findings — Edit page (captured 2026-08-28, [capture file](captures/2026-08-28-stereo-timeline-edit-page.log)):**

**The right eye never rendered. All 92 render calls in the session were `eye=L`** — across scrubbing, parked re-renders, and true playback. The capture decodes into three phases:

- Scrub/background activity (`#11–39`, `#52–72`): times oscillating non-monotonically over ~7700–10100 at ~300 ms spacing.
- Parked (`#40–51`): frame 8310 re-rendered 12× (param edits re-render the parked frame), with multi-second idle gaps.
- **Linear playback (`#77–92`): `time` strictly monotonic +1 (7665→7680) at ~230 ms/frame.** Resolve rendered every timeline frame sequentially — render-rate-limited well below real time at 4096×4096, no frame drops — and still only ever the left eye.

Supporting observations from the same capture:

- `dim=4096x4096` throughout — single-eye-sized frames; no packed/double-width frame ever reached the seam (consistent with the feasibility study §1.2).
- Two `dim=184x92 thumb=1` lines — filmstrip thumbnail renders hit the render action and the thumbnail flag correctly identifies them (the later "skip thumbnails" work is both needed and possible).
- `page=?` on all calls — **Resolve 21.0.4 did not deliver `OfxImageEffectPropResolvePage` at createInstance** for this instance (created at project load), contradicting the shipped header's stated delivery point. Needs a re-test with the effect freshly added from the UI on a known page.
- `src − time = 215400` constant on every line — `src` tracks the timeline frame exactly (clip offset), so it remains the candidate L/R pairing key if per-eye calls ever appear.

(That first capture ran with the Stereo 3D palette at **Vision: Mono**. The follow-up below flips it. Throughout, `dt` includes the plugin's own full-frame processing at 4096², so treat ~230 ms as an upper bound on host cadence, not a measurement of it.)

**Findings — Stereo 3D palette Vision: Stereo (captured 2026-08-28, [Edit page](captures/2026-08-28-stereo-timeline-edit-page-vision-stereo.log), [Color page](captures/2026-08-28-stereo-timeline-color-page.log)):**

Flipping the Color-page **Stereo 3D palette from Vision: Mono to Vision: Stereo** — with **Out left at None** — changes the seam completely, and answers the key unknown in the affirmative:

- **Both eyes render, every frame, on the Edit page and the Color page alike.** 26/26 (Edit) and 24/24 (Color) perfectly paired L/R calls, with clean monotonic playback stretches in both captures (7717→7739, 7765→7787). Still never a packed frame — each call is a single eye at 4096×4096.
- **Each eye renders through its own plugin instance.** The probe counters expose two instances: the original one (left eye — its counter continued #110+ from the first capture) and a second, right-eye instance Resolve created when Vision went Stereo (counter from ~#1). Both persist across page switches. **Instance-local state can never pair eyes — the pairer must be process-global.**
- **Pairing key confirmed:** the two calls of a pair share `time` (and `src`) exactly; R renders first, L follows by 28–71 ms (avg 32 ms); zero unpaired frames across all 50 pairs.
- Combined with the Vision: Mono capture: the stereo tap's operating requirement is just "set the project to Stereo vision" — no mux Out mode needed.

**Casualty discovered — the NDI feed went black in stereo mode.** The unified log at the time shows both instances in a failure loop, in R/L pair cadence: `NDIlib_send_create` failing on every render attempt ("Failed to create NDI sender…"; the name itself logs as `<private>` — os_log redaction). Reading the code against the log: both instances create senders with the **identical default source name** (duplicate-name registration is the prime suspect for the second create failing), and `initializeNDI`'s failure path calls **`NDIlib_destroy()`** — tearing the process-wide NDI library out from under the other instance's healthy sender. The failing instance retries every frame, so the healthy one can never recover. First hard requirement for the stereo work (issue #6): process-shared NDI lifecycle — single sender ownership (or eye-suffixed names) and no global `NDIlib_destroy()` while another instance is live.

### Scenario 3 — Resolve 21 "Standard Immersive" VR180 project

- **Slug:** `vr180-standard-immersive`
- **Setup:** new project with Resolve 21's **Standard Immersive** (VR180) project/timeline type and stereo VR180 media. NDIOutput on an Edit-page adjustment clip (add a Color-page capture if behavior differs). Play ~15 s.
- **Questions:** Does the immersive seam deliver **per-eye renders** (`eye=L`/`eye=R`, one eye's dimensions) or **packed frames** (no/`L`-only eye, double-width or double-height `dim=`)? This decides which plugin mode LSVR's equirect work needs.

**Findings: PER-EYE RENDERS, never packed — confirmed on two immersive project types.**

1. **VR180 (4096×4096/eye):** the scenario 2 captures above *were* a VR180 project (see correction at top) — per-eye instances, both eyes when Vision: Stereo, single-eye `dim=4096x4096` on every call.
2. **Apple Immersive pipeline (8160×7200/eye)** ([capture](captures/2026-08-28-apple-immersive-8160x7200.log), 70 calls): the seam delivers **full-resolution 8160×7200 single-eye frames** — `scale=1.00`, both eyes, again via two per-eye plugin instances. At float32 RGBA that is ~940 MB per eye per frame handed to the plugin, which is the hard evidence behind issue #5's downscale-before-readback requirement (and Matt's product call: the plugin must offer stream downscaling, Nobe-style). Observed cadence ~72–76 ms per eye (~13 fps effective) — Resolve kept feeding full-rate sequential frames; the ceiling is processing, not the seam.

**Pairing is not orderly at 8K — hard requirements for issue #6's pairer,** from the Apple Immersive capture: 31 frames arrived as L/R pairs sharing `time`/`src`, but arrival offset ranged **−334 ms to +78 ms** (avg R leads by 8 ms; **L arrived first in 4 pairs**); `time` went **backwards within a single eye** 4 times mid-stream (e.g. R rendered 8202 before 8201); 3 R-only and 1 L-only frames never got a mate in-window; and after stopping, both instances re-rendered the parked frame repeatedly. So the pairer must be: keyed on `time` (not arrival order), buffered over a several-frame reorder window (≥ ~5 frames at this cadence), duplicate-tolerant (parked re-renders replace the held frame), and timeout-dropping for unmated frames — strict R-then-L alternation, which the 4096² captures suggested, does not survive 8K.

### Scenario 4 — Timeline Proxy Mode

- **Slug:** `proxy-mode-half`
- **Setup:** any timeline from above; **Playback → Timeline Proxy Resolution → Half** (then Quarter for a second pass). Play ~10 s each.
- **Questions:** Confirm the plugin receives half/quarter-size frames. Does the host report it via `scale=` (0.50/0.25), via shrunken `dim=`, or both? Documents the "why did the stream go soft" support case.

**Findings: DESCOPED (Matt, 2026-08-28).** Proxy-mode behavior stays documented from secondary sources only (Nobe FAQ / Resolve manual — the plugin receives proxy-sized frames; see the pitfalls table in [LEARNINGS.md](../LEARNINGS.md) §2). The product direction replacing this test: **the plugin itself must offer stream downscaling** (Full/Half/Quarter, Nobe-style) so the outgoing NDI resolution is a deliberate choice rather than whatever the host happens to feed — issue #5, now backed by the 8160×7200 findings above. If a support case ever hinges on how proxy mode is reported (`scale=` vs `dim=`), it's a 2-minute capture with the probe.

## 4. Overhead check (acceptance criterion)

With **Log Render Calls off**, the probe costs one boolean parameter read and one branch per render call — in a render path that already does per-frame parameter reads, a full-frame memcpy, and color conversion. Confirm during any capture session: toggle it off mid-playback and verify playback fps and NDI output are unchanged.

## 5. Summary of answers (fill in when captures land)

| Question | Answer | Evidence |
|---|---|---|
| Second eye during Edit-page playback? | **Depends on Stereo 3D palette Vision.** Vision: Mono → left only, always. Vision: Stereo (Out: None suffices) → **both eyes, every frame**, via a second per-eye plugin instance | [mono-vision](captures/2026-08-28-stereo-timeline-edit-page.log), [stereo-vision](captures/2026-08-28-stereo-timeline-edit-page-vision-stereo.log) |
| Second eye on Color page / with palette armed? | **Identical to Edit page** — same instances, same pairing, same cadence | [color-page](captures/2026-08-28-stereo-timeline-color-page.log) |
| Pairing key for L/R (same `time`? same `src`?) | **Both** — pairs share `time` and `src` exactly, arriving on **separate instances** → pairing must be process-global. At 4096²: R first, L +32 ms avg, 50/50 matched. **At 8160×7200 order breaks down**: either eye can lead (−334…+78 ms), `time` reverses within one eye, unmated frames occur → time-keyed reorder buffer, not arrival order | stereo-vision + apple-immersive captures |
| VR180 / immersive: per-eye or packed? | **Per-eye, never packed** — single-eye frames at 4096×4096 (VR180) and full 8160×7200 (Apple Immersive, ~940 MB/eye as float32 — issue #5's downscale-before-readback is mandatory) | [apple-immersive](captures/2026-08-28-apple-immersive-8160x7200.log) + scenario 2 captures |
| Proxy mode: `scale=` vs `dim=`? | **Descoped** — plugin-side downscale (Full/Half/Quarter, Nobe-style) is the product answer instead → issue #5 | scenario 4 note |
| Page property actually delivered at createInstance? | **No** — `page=?` on all 192 calls, including the freshly created right-eye instance; don't build on this property | all captures |
| Thumbnail renders observed (`thumb=1`)? | **Yes** — 184×92 filmstrip renders, correctly flagged | mono-vision capture |
| NDI output usable in stereo mode today? | **Fixed in v1.5.0, Tier 1–2 validated 2026-08-28** (was: feed went black — second instance's sender create failed on the duplicate name and its failure path `NDIlib_destroy()`d the library under the healthy sender). Senders are now process-shared/refcounted, `NDIlib_destroy()` is never called, and the eyes pair into one packed SbS/TB frame — issue #6 | unified log 12:08, scenario 2 findings; LEARNINGS.md 2026-08-28 v1.5.0 entries |
