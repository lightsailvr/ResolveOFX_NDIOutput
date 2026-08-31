# Vendored NDI SDK pieces (CI compile support only)

The NDI Advanced SDK download is access-gated, so hosted CI cannot install
it. This directory carries the minimum needed for CI to *compile and link*
the plugin without it (see `CMakeLists.txt`, "CI mode"):

- `include/` — the `Processing.NDI.*.h` headers from the **NDI 6 Advanced
  SDK** (copied 2026-08-30 from an installed SDK). Each file carries its own
  MIT license notice from Vizrt NDI AB ("The following MIT license applies to
  this file ONLY and not to the SDK as a whole"), which is what permits
  checking them in. No non-MIT SDK file may be added here.
- `Processing.NDI.Lib.Advanced.x64.def` — the export-name list of
  `Processing.NDI.Lib.Advanced.x64.dll`, extracted with
  `scripts/dump_ndi_exports.py`. CMake turns it into a stub import library so
  the linker can resolve NDI calls. The stub contains no NDI code, is never
  shipped, and never loads at runtime.

Builds on a machine with the real SDK installed (the default
`NDI_SDK_PATH`) use the SDK's own headers and import library instead; any
binary that actually streams must be built that way, and release builds must
use an SDK no more than 30 days old (NDI Advanced License Agreement §2.b —
see docs/windows-port-spec.md, decision 13).

When a new SDK version lands, refresh both the headers and the .def:

```bash
python scripts/dump_ndi_exports.py "C:/Program Files/NDI/NDI 6 Advanced SDK/Bin/x64/Processing.NDI.Lib.Advanced.x64.dll"
```

NDI® is a registered trademark of Vizrt NDI AB — https://ndi.video
