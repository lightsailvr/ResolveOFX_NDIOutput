# Render-Call Probe Findings: What Resolve Feeds the OFX Render Action

**Date started:** 2026-08-28 · **Plugin:** NDIOutput ≥ v1.3.0 · **Host:** DaVinci Resolve Studio 21.0.4, macOS 26.x
**Issue:** [#3](https://github.com/lightsailvr/ResolveOFX_NDIOutput/issues/3) — instrument every render call (eye, page, thumbnail, cadence) so host behavior is observed, not guessed.

**Status: instrumentation shipped (v1.3.0); scenario 2 Edit-page captured — headline: the right eye never renders on the Edit page.** Remaining: scenario 2 Color page + palette-armed variant, scenarios 1, 3, 4. Each capture needs a human at the Resolve machine — playback cannot be scripted (the Resolve API has no transport control, see [LEARNINGS.md](../LEARNINGS.md) §2). Fill in each *Findings* section from the capture files and flip this line to **complete** when all four are in.

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

**Findings:** _pending capture_

### Scenario 2 — native stereoscopic timeline, Edit page vs Color page ⭐ the key unknown

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

**Still pending for this scenario:** the Color-page capture, and the variant with the Stereo 3D palette Out mode armed (Side by Side) — the last hope for forcing right-eye renders on the Edit page. A longer playback stretch (≥15 s) would also firm up the cadence numbers; `dt` here includes the plugin's own ~full-frame processing at 4096², so treat ~230 ms as an upper bound on host cadence, not a measurement of it.

### Scenario 3 — Resolve 21 "Standard Immersive" VR180 project

- **Slug:** `vr180-standard-immersive`
- **Setup:** new project with Resolve 21's **Standard Immersive** (VR180) project/timeline type and stereo VR180 media. NDIOutput on an Edit-page adjustment clip (add a Color-page capture if behavior differs). Play ~15 s.
- **Questions:** Does the immersive seam deliver **per-eye renders** (`eye=L`/`eye=R`, one eye's dimensions) or **packed frames** (no/`L`-only eye, double-width or double-height `dim=`)? This decides which plugin mode LSVR's equirect work needs.

**Findings:** _pending capture_

### Scenario 4 — Timeline Proxy Mode

- **Slug:** `proxy-mode-half`
- **Setup:** any timeline from above; **Playback → Timeline Proxy Resolution → Half** (then Quarter for a second pass). Play ~10 s each.
- **Questions:** Confirm the plugin receives half/quarter-size frames. Does the host report it via `scale=` (0.50/0.25), via shrunken `dim=`, or both? Documents the "why did the stream go soft" support case.

**Findings:** _pending capture_

## 4. Overhead check (acceptance criterion)

With **Log Render Calls off**, the probe costs one boolean parameter read and one branch per render call — in a render path that already does per-frame parameter reads, a full-frame memcpy, and color conversion. Confirm during any capture session: toggle it off mid-playback and verify playback fps and NDI output are unchanged.

## 5. Summary of answers (fill in when captures land)

| Question | Answer | Evidence |
|---|---|---|
| Second eye during Edit-page playback? | **No — left eye only**, incl. a 16-frame monotonic playback stretch (pending confirmation with palette armed) | [stereo-timeline-edit-page](captures/2026-08-28-stereo-timeline-edit-page.log), scenario 2 findings |
| Second eye on Color page / with palette armed? | _pending_ | |
| Pairing key for L/R (same `time`? same `src`?) | _pending R sightings_ — `src` tracks `time` exactly (constant clip offset), so it's the candidate | same capture |
| VR180 Standard Immersive: per-eye or packed? | _pending_ | |
| Proxy mode: `scale=` vs `dim=`? | _pending_ | |
| Page property actually delivered at createInstance? | **No** (instance created at project load) — `page=?` on all calls; retest with effect freshly added per page | same capture |
| Thumbnail renders observed (`thumb=1`)? | **Yes** — 184×92 filmstrip renders, correctly flagged | same capture |
