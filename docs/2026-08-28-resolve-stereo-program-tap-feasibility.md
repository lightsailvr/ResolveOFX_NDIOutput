# Feasibility: A Reliable Stereo Program Feed out of DaVinci Resolve for VR Headset Monitoring

**Date:** 2026-08-28 · **Researched on:** the LSVR post machine (DaVinci Resolve Studio 21.0.4, macOS 26.x, Nobe Display Connect 2.0.32, NDI Advanced SDK 6.2.0 + NDI SDK 6.3.2 installed)
**Question:** Can we get a side-by-side (or stacked) stereoscopic program feed out of Resolve's **Edit page** onto NDI or another low-latency stream, with selectable projection (equirect / Apple Immersive lens space) in the headset, reliably?

**Priority (Matt, 2026-08-28): Quest-first.** AVP is heavy, cumbersome, and too expensive to distribute to the team; the Vision Pro paths below are retained for Apple Immersive deliverable QC only. This favors the NDI architecture strongly: the Quest receiver (`vrndi_quest` / VR.NDI Untethered) already ships, and Quest's full-bandwidth-NDI-only constraint means Phase 1 needs no HX/HEVC transport work at all.

**Verdict: Yes — highly feasible, and LSVR already owns most of the stack.** The one seam Blackmagic exposes to live playback frames is the OpenFX render action, and Resolve's shipped SDK contains an undocumented-but-implemented stereo extension (`kOfxImageEffectPropEyeToRender`) that tags every render call left/right eye. Upgrading `LSVR_ResolveOFX_NDIOutput` to pair eyes, pack SbS/stacked on GPU, and send at a monitoring-grade resolution is the robust build. Two zero-code paths (DeckLink stereo mux from the Edit page, and Video Clean Feed capture) are viable stopgaps/backstops. Tapping Blackmagic's own Remote Monitoring stream is **not** viable (WebRTC with proprietary signaling, end-to-end encrypted, no third-party client has ever been demonstrated).

Labels: **(P)** verified primary source · **(U)** user-reported/secondary · **(T)** needs testing · **(L)** verified locally on this machine.

---

## 1. Why each current approach fails

### 1.1 "Stream to visionOS" (Apple Immersive preview) crashes
- The feature (Workspace → Stream to visionOS, from Edit/Color/Fusion/Deliver pages) shipped in Resolve Studio **20.1** (2025-08-07) and streams to Apple's **AIVU** app on the Vision Pro — not to DaVinci Remote Monitor. Wi-Fi 5 min/Wi-Fi 6 preferred, or **wired via the Developer Strap**. (P: Resolve 21 manual pp. 4363–4407; release notes forum.blackmagicdesign.com t=225417)
- Instability is corroborated and **unfixed through 21.0.4**: connection/discovery failures (t=225556), and on Resolve 21 an official-forum thread (t=238260, Jul–Aug 2026) reports 360° content playing as 180°, AIVU covering the Mac Virtual Display, and "I'm using the developer strap and the connection is much more stable than with WiFi, **but it still drops**." No release note through 21.0.4 claims a stability fix for this feature. (U/P)
- Known working conditions from user reports: AIVU must be open **only on the Vision Pro** (not on the Mac), and the timeline must be playing for the stream icon to appear. (U)
- Apple's own adjacent path sets the quality bar: AIVU 1.2 real-time streaming from a Mac is **MV-HEVC 3600×3600@45 fps** (macOS 26.1 + visionOS 26.1), vs. 4320×4320@90 for file playback. (P: support.apple.com/guide/immersive-video-utility/dev4579429f0)
- **Strategic watch item:** Apple announced visionOS 27 ImmersiveMediaSupport will add "real-time previewing of Apple Immersive Video on Apple Vision Pro directly from a Mac during editorial or live production" plus custom compositor pipelines. The AIV/fisheye monitoring case may get an OS-level fix; equirect/VR180 and open-transport monitoring remain the gap. (P: developer.apple.com/visionos/whats-new/)

### 1.2 Nobe Display fails on stacked stereoscopic clips
- Root cause confirmed from two directions:
  1. On a native stereo timeline, Resolve renders **each eye as a separate single-eye OFX render call**, tagged via the Resolve-specific property `kOfxImageEffectPropEyeToRender` (`OfxImageEye` = left/right). Verified in the shipped header `/Library/Application Support/Blackmagic Design/DaVinci Resolve/Developer/OpenFX/OpenFX-1.4/include/ofxImageEffectExt.h` **and present as a string in the Resolve 21.0.4 host binary**, so the host implements it. An OFX plugin never receives a packed L+R frame from a stereo timeline. (P/L)
  2. Nobe's own docs: in "DaVinci Resolve's native stereoscopic or Apple Immersive timeline modes… the OpenFX plugin path may receive only a single eye rather than the full packed stereo output," and they recommend hardware I/O for critical stereo monitoring. SBS left/right-eye **pairing** was only added in Nobe 2.0.26b (2026-05-05); **stacked/over-under pairing appears nowhere** in docs or changelog. (P: docs.timeinpixels.com/nobe-display/stereoscopic-vr180; timeinpixels.com/nobe-display-changelog)
- The packed SbS image the headset needs is composed **downstream of every OFX node**, at Resolve's video-I/O output stage (Color-page Stereo 3D palette → Out mode) — which is also why the projection "only shows correctly in the output when in the color page." Kara's BMD forum thread (2026-01-10, t=232249) documents exactly this; Peter Chamberlain (BMD) engaged with questions only, no fix as of Aug 2026. (P/U)
- Additional OFX-tap failure modes documented by Time in Pixels and the manual: Smart/Render Cache caches OFX nodes and **stops firing the render action** (frozen feed); background caching fires renders while parked (phantom feed); Timeline Proxy Mode silently halves/quarters the frames the plugin sees. (P: Nobe FAQ; Resolve 21 manual ch. 8)
- Note: installed Nobe here is 2.0.32 (> 2.0.26b), so its **SbS pairing on stereo timelines is worth a retest** on 21.0.4 — but stacked remains unsupported. (T)

### 1.3 DeckLink output: 4K DCI reframing, and (reportedly) color-page-only SbS
- Raster coercion is by design: the Video Monitoring settings only offer **standard video rasters**, so a square/immersive timeline cannot go out SDI/HDMI natively; BMD's own immersive guide says to monitor at HD/UHD while the timeline runs 8160×7200@90. The specific fall-back choice of 4096×2160 DCI is undocumented behavior (nearest-raster heuristic). (P: manual pp. 145, 4365)
- **Correction worth money: the manual states (since Resolve 19) that muxed stereo over DeckLink/UltraStudio works from the Edit page.** "The Edit Page Viewer itself still shows only a single eye, but it now displays Stereoscopic 3D video from the DeckLink or Ultrastudio video outputs" — with the mux mode (Side by Side / Line by Line / etc.) armed from the Color-page Stereo 3D palette. Furthermore, **20.3.1 (2025-12-18) "Addressed an issue with stereoscopic 3D video monitoring output"** — the color-page-only behavior LSVR saw may have been this bug. Retest on 21.0.4 before ruling this path out. (P: manual p. 327; release notes t=231515) (T)
- Dual-SDI full-res per-eye output also exists ("Use dual outputs on SDI" / "Use left and right eye SDI output," 4:2:2 Y'CbCr per eye). (P: manual pp. 145, 322, 338)

---

## 2. The complete map of Resolve's real-time output taps

| Tap | Live frames? | Stereo? | Edit page? | Openness | Notes |
|---|---|---|---|---|---|
| **OpenFX render action** | Yes — the only SDK seam in the playback loop | Per-eye calls tagged `EyeToRender`; packed frames only if media is pre-packed in a mono timeline | Yes (clip or adjustment clip; `kOfxImageEffectPropResolvePage` tells you which page) | Open (SDK on disk) | Caching/proxy caveats; GPU path = float32 RGBA MTLBuffer (Metal-only on macOS since 20.3) |
| **DeckLink/UltraStudio** | Yes (true program out) | Yes — muxed SbS/LbL from any page once armed; or dual-SDI per eye | Yes (per manual since R19; 20.3.1 bugfix) | Open (SDI) | Standard rasters only → anamorphic squeeze for square content |
| **Video Clean Feed** | Yes (GUI viewer fullscreen on 2nd display) | Shows whatever the viewer shows: stacked media in mono timelines = both eyes; stereo timelines = one eye unless palette Out mode set | Yes (Media/Cut/Edit/Fusion/Fairlight/Color) | Capture-able (ScreenCaptureKit/OBS→NDI, or HDMI capture) | Studio-only; display-raster-bound; BMD PM's own suggestion for 8K immersive viewing (t=236792) |
| **DaVinci Remote Monitor** | Yes (viewer stream) | No stereo projection; AVP client is a flat window | Yes (Media/Cut/Edit/Color/Deliver, +Fusion previews in 21) | **Closed**: WebRTC (GStreamer webrtcbin + Qt WebSockets + mbedTLS in the binary), H.264/H.265 up to UHD 12-bit 4:4:4, E2E-encrypted per BMD staff; IP mode on TCP 16410 + TCP/UDP 16411+; zero public protocol docs, no known third-party client | Not a build target |
| **Stream to visionOS** | Yes (immersive) | Yes (MV-HEVC to AIVU) | Yes (Edit/Color/Fusion/Deliver) | Closed (Apple AIVU ecosystem) | AIV/fisheye projects only; unstable through 21.0.4; watch visionOS 27 |
| Scripting / Workflow Integration / IO Encode SDKs | No (stills, control, render-time only; IO Encode is CPU-only) | — | — | Open | Confirmed no live-frame access (P: on-disk READMEs) |

---

## 3. Receiver-side reality (headsets)

- **NDI SDK 6.2+ officially supports visionOS**, including receive and **hardware HX decode via VideoToolbox** on macOS/iOS/visionOS. On **Android (Quest), HX decode is not in the SDK** — full-bandwidth NDI only (compressed pass-through requires rolling your own decode). This is why VR.NDI Untethered's practical ceiling is ~4K. (P: docs.ndi.video platform-considerations, decoding guide)
- **Quest 3 hardware decode ceiling** (Meta docs, 2026-04): SbS 180-3D realistic 7680×3840@60, max **8192×4096@60**; H.265 or AV1. The Quest app's existing 8192×4096 cap is already at the hardware limit. (P)
- **Vision Pro is Wi-Fi 6 only** (all models). Apple's own live monitor runs MV-HEVC 3600×3600@45 inside that budget; USB-C Developer Strap is the wired fallback. AVP file playback proves out far higher (4320×4320@90 MV-HEVC; community tests to ~7500×7500/eye@30). (P/U)
- **Sanctioned custom live stereo player API on visionOS:** per-eye `CMTaggedBuffer`s (`.stereoView(.leftEye/.rightEye)`) into `AVSampleBufferVideoRenderer`, usable with RealityKit `VideoMaterial`/`VideoPlayerComponent` — Apple engineer-confirmed for live rendering. This is the VR.NDI visionOS render path. (P: developer.apple.com/forums/thread/795745)
- **Transports that meet <200 ms on LAN:** NDI High Bandwidth (sub-frame), NDI HX3 (vendor <100 ms; ~50–110 Mbps at 2160p60), tuned SRT (~100–250 ms), WebRTC (~sub-150 ms achievable). **LL-HLS cannot** (~2 s floor) — APMP/HLS is a review/dailies transport, not a scrub-monitor. (P/U)
- **Existing comparables:** QTAKE Monitor does live stereo + immersive-180 4K to AVP at a claimed ~1.5 frames (requires QTAKE video-assist ecosystem + STREAM module). `sr2.tech/immersivemonitor.html` sells an NDI-based URSA Cine Immersive live monitor for AVP/iPad/Mac (direct comparable to VR.NDI's on-set use case — worth a look). "Spatial Render" ($99, Mac App Store) streams any Mac screen/HDMI capture to AVP with VR180-3D-SBS projection at 2.5K/eye@90 — a cheap instant experiment on top of Video Clean Feed. Vxio (free, visionOS) does NDI with 180/360 immersive modes but stereo is undocumented. Sienna NDI Monitor (AVP) is flat-only. (P/U)
- **NDI licensing:** receive-only apps ride the free standard SDK (with NDI branding requirements). **HX encode and HDR send require the NDI Advanced SDK commercial license** — the Advanced SDK is already installed and linked by the LSVR plugin; confirm commercial terms with Vizrt before shipping HX-encoding builds. (P: docs.ndi.video licensing)

---

## 4. Ranked architectures

### A. Upgrade `LSVR_ResolveOFX_NDIOutput` into a stereo-aware program tap — **the build. Feasibility: HIGH**
The OFX render action is the only supported live-frame seam, the eye tag exists in the host, and every downstream piece (StereoMuxBox pairing logic, Quest receiver, VR.NDI Apple app design incl. ILPD→STMap lens pipeline) already exists in-house.

Required upgrades (all grounded in findings):
1. **Stereo handling, both modes:**
   - *Stereo/immersive timelines:* read `kOfxImageEffectPropEyeToRender` per render call; buffer left, pair with right at the same frame time (`kOfxImageEffectPropSrcFrame` helps match), pack **SbS or stacked (user choice)** on GPU, send one NDI frame per pair. Reuse StereoMuxBox's FramePairer/StereoCompositor logic.
   - *Mono timelines with pre-packed (stacked/SbS) media:* passthrough as today.
2. **GPU-native path:** declare `kOfxImageEffectPropMetalRenderSupported` and consume the host's float32 RGBA `MTLBuffer` directly; **downscale to the target stream resolution on GPU before any readback** (an 8160×7200 float32 eye is ~940 MB — readback at timeline res is the current design's killer). Convert to UYVY/P216 on GPU (kernels already exist in the repo), then one small readback → NDI.
3. **Resolve-host hygiene:** skip thumbnail renders (`kOfxImageClipPropThumbnail`); expose which page instantiated the effect (`kOfxImageEffectPropResolvePage`); derive frame rate from the host (current default of 25 fps is wrong for 90 fps immersive); detect render-cache starvation (frame-time watchdog → UI status "Resolve is serving cached frames — set Render Cache to None").
4. **Stream sizing for the receiver fleet (Quest-first):** default **1920–2160/eye → SbS 3840×1920 or 4320×2160** — inside the Quest app's ~4K practical ceiling for full-bandwidth NDI (the limiter is SpeedHQ *CPU* decode on the headset plus Wi-Fi bandwidth ~130–250 Mbps, not the 8192×4096@60 hardware decode ceiling). Multiple Quests = one NDI stream per receiver; 2–3 headsets is realistic on a dedicated Wi-Fi 6E AP. Apple's own 3600²@45 monitoring bar says full 4320²@90 is unnecessary for creative monitoring anyway.
5. **Optional sender-side projection normalization:** add a Metal STMap-warp mode in the plugin (same Fusion-authored EXR STMaps the VR.NDI PRD uses as its Apple-independent path) so fisheye/lens-space timelines can be converted to **equirect on the Mac before streaming**. Then every receiver — including the existing Quest app's Stereo-180 mode — displays it correctly with zero receiver changes, and "lens space vs equirect" becomes a sender toggle. Alternative: ship the raw lens-space frame and add an STMap warp shader to the Unity app (EXR STMap → texture; keeps native-space pixels for QC).
6. **Phase-2 transport (only when >4K/eye matters):** VideoToolbox low-latency HEVC → RTP/SRT → Quest MediaCodec hardware decode raises the ceiling toward the 8192×4096@60 hardware limit at ~50–100 Mbps. (NDI HX3 does *not* help Quest — Android SDK can't decode HX.) Evidence supports real-time 8K30-class HEVC encode on M-series; 8K60 unproven — validate on the Studio.

Risks: `EyeToRender` is header-documented but publicly uncommented by BMD (no forum footprint; could change — pin tests per Resolve release); cache/proxy interactions need the hygiene features; per-eye render cadence on Edit vs Color page must be characterized (test matrix below).

### B. First-party DeckLink stereo mux from the Edit page → NDI encoder — **zero-code stopgap. Feasibility: MEDIUM-HIGH, test first**
Arm Color-page Stereo 3D palette (Vision: Stereo, Out: Side by Side) on 21.0.4, work on the Edit page, DeckLink/UltraStudio outputs muxed SbS at a standard raster (each eye anamorphic — e.g. 1920×2160/eye in UHD); hardware-encode to NDI (Kiloview-class box, as in the VR.NDI camera pipeline) → existing receivers. The manual says this works since R19; a stereo-monitoring output bug was fixed in 20.3.1 — LSVR's experience predates that fix. Resolution-capped and anamorphic, but rock-solid latency and reliability if it retests clean. Dual-SDI per-eye + 2× capture + StereoMuxBox is the full-res variant of the same idea (that's effectively the Jan-2026 UltraStudio→OmniScope workaround, done properly).

### C. Video Clean Feed capture — **fallback/QC path. Feasibility: MEDIUM**
Clean feed mirrors the current viewer full-screen on a second display from every page; with stacked media in a mono timeline it shows both eyes. Capture via ScreenCaptureKit/OBS→NDI (or HDMI capture on a dummy display). Display-raster-bound (≈4K–6K), ~2–4 frames extra latency, zero code. Also the cheapest instant AVP experiment: point **Spatial Render** at the clean-feed display.

### D. Stream to visionOS / AIVU — **keep for AIV finishing checks; don't build on it**
Fisheye/AIV-only, AVP-only, unstable through 21.0.4 (drops even on the developer strap, per user reports). Re-evaluate at each Resolve point release and when visionOS 27's ImmersiveMediaSupport real-time preview ships.

### E. Tap the Remote Monitoring stream — **not viable**
WebRTC with proprietary WebSocket signaling, E2E encryption (BMD-staff-confirmed), stripped/obfuscated `libdavstream`, zero public reverse-engineering after extensive search, and its AVP client is flat-only anyway. Any successful tap would be fragile against point releases. The plumbing is real but sealed.

---

## 5. Test matrix (Phase 0 — no code, ~half a day)

| # | Test | What it decides |
|---|---|---|
| 1 | 21.0.4 + stereo timeline + Color-palette SbS armed + work on **Edit page** → DeckLink out | Is architecture B live today? (20.3.1 fixed a bug here) |
| 2 | Same timeline, Nobe 2.0.32/2.0.33 SBS mode on Edit-page adjustment clip | Did Nobe's May-2026 eye-pairing fix LSVR's SbS case? (Stacked will still fail) |
| 3 | Current LSVR plugin on an Edit-page **adjustment clip**, mono timeline with stacked 4320×2160 SbS media, Render Cache=None, proxy off | Confirms the passthrough path + measures perf/latency baseline |
| 4 | Same as 3 on a **stereo** timeline — log render calls (count, cadence, which eye if any) on Edit vs Color page | Characterizes when Resolve renders the second eye — the key unknown for architecture A |
| 5 | R21 "Standard Immersive" VR180 project: is the timeline stereo-per-eye or packed at the OFX seam? | Which plugin mode LSVR's equirect work needs |
| 6 | Stream to visionOS via **Developer Strap** on 21.0.4 + macOS 26.1/visionOS 26.1 + AIVU 1.2, AIVU closed on the Mac, timeline playing | Whether the first-party path is now tolerable for AIV checks |
| 7 | Timeline Proxy Mode on: confirm OFX receives half-res | Documents the "why did the stream go soft" support case |

## 6. Open questions
- Per-eye render cadence on Edit page during playback (does the non-displayed eye render every frame? does arming the palette SbS Out force both?). Test #4.
- Whether Resolve's OFX host offers half-float GPU images (samples are float32-only; undocumented).
- NDI Advanced SDK commercial terms for shipping HX-encoding builds (receive-only is free).
- visionOS 27 IMS real-time preview scope — if it covers editorial AIV preview reliably, architecture D re-ranks for the fisheye case.

## 7. Key sources
- Resolve 21 manual (local: `/Applications/DaVinci Resolve/DaVinci Resolve Manual.pdf`): Remote Monitoring ch. 198 (pp. 4343–4350, WebRTC + ports); Stereo ch. 14 (pp. 320–339, Edit-page stereo out p. 327); Apple Immersive ch. 200 (pp. 4359–4408); Clean Feed p. 62.
- SDK header (local): `/Library/Application Support/Blackmagic Design/DaVinci Resolve/Developer/OpenFX/OpenFX-1.4/include/ofxImageEffectExt.h` (`EyeToRender`, `ResolvePage`, `SrcFrame`, `Thumbnail`); property strings confirmed in the Resolve 21.0.4 binary.
- Binary analysis (local): `Blackmagic Remote Monitor.app` links GStreamer webrtcbin/RTP/SDP, Qt WebSockets, mbedTLS, H264/H265 parsers.
- Release notes: t=225417 (20.1, visionOS streaming), t=231515 (20.3.1 stereo monitoring fix), t=236904 (21.0 Standard Immersive/VR180-360), t=238983 (21.0.4 current).
- Nobe: docs.timeinpixels.com/nobe-display/stereoscopic-vr180 (single-eye admission), changelog (2.0.26b SBS pairing).
- LSVR's own BMD thread: forum.blackmagicdesign.com t=232249 (Kara Lancaster, 2026-01-10).
- NDI: docs.ndi.video (visionOS in 6.2; no Android HX decode; HX3 specs; licensing).
- Meta media specs: developers.meta.com …/media-requirements.md (8192×4096@60 decode ceiling).
- Apple: AIVU requirements (3600²@45 live), developer.apple.com/visionos/whats-new/ (visionOS 27 IMS real-time preview), forums thread/795745 (tagged-buffer live stereo rendering).
- Comparables: monitor.qtake.app · sr2.tech/immersivemonitor.html · Spatial Render (Mac App Store id6751405237) · Vxio (id6475702074).
