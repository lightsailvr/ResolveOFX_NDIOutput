# Render-Call Probe Findings: What Resolve Feeds the OFX Render Action

**Date started:** 2026-08-28 · **Plugin:** NDIOutput ≥ v1.3.0 · **Host:** DaVinci Resolve Studio 21.0.4, macOS 26.x
**Issue:** [#3](https://github.com/lightsailvr/ResolveOFX_NDIOutput/issues/3) — instrument every render call (eye, page, thumbnail, cadence) so host behavior is observed, not guessed.

**Status: instrumentation shipped (v1.3.0); all four captures PENDING.** Each scenario needs a human at the Resolve machine — playback cannot be scripted (the Resolve API has no transport control, see [LEARNINGS.md](../LEARNINGS.md) §2). Fill in each *Findings* section from the capture files and flip this line to **complete**.

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

**Findings:** _pending capture_

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
| Second eye during Edit-page playback? | _pending_ | |
| Second eye on Color page / with palette armed? | _pending_ | |
| Pairing key for L/R (same `time`? same `src`?) | _pending_ | |
| VR180 Standard Immersive: per-eye or packed? | _pending_ | |
| Proxy mode: `scale=` vs `dim=`? | _pending_ | |
| Page property actually delivered at createInstance? | _pending_ | |
| Thumbnail renders observed (`thumb=1`)? | _pending_ | |
