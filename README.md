# NDI Advanced Output Plugin for DaVinci Resolve

A modern OpenFX plugin that sends video frames from DaVinci Resolve to NDI (Network Device Interface) for streaming over network with comprehensive HDR support.

> **Download**: grab the signed installer from the [latest release](https://github.com/lightsailvr/ResolveOFX_NDIOutput/releases/latest) — no SDK or other software needed.  
> **Build & Install from source**: [BUILD.md](BUILD.md) · **Dev workflow & testing loop**: [LEARNINGS.md](LEARNINGS.md)

## Features

- **Modern OFX Implementation**: Uses C API directly for maximum compatibility with DaVinci Resolve 20+
- **HDR Support**: Full HDR workflow support with:
  - PQ (ST.2084) and HLG (Hybrid Log-Gamma) transfer functions
  - Rec.2020, DCI-P3, and Rec.709 color spaces
  - Configurable Max Content Light Level (CLL) and Max Frame Average Light Level (FALL)
  - HDR metadata embedding for proper downstream handling
- **Stereo Timelines**: On a native stereo timeline (Stereo 3D palette at **Vision: Stereo**), the plugin pairs the left/right eye renders per frame and streams **one packed frame** — Side-by-Side or Top-Bottom (Stereo group) — for VR receivers such as VR.NDI on Quest. The packed canvas is locked for the stream's lifetime: brief eye stalls re-send the last packed frame, and a sustained stall degrades to the flowing eye duplicated into both halves (labeled in the **Stream Status** parameter) — the frame dimensions never change mid-stream, so the geometry never pops in-headset. Mono timelines are unaffected.
- **NDI Advanced SDK**: Uses NDI Advanced SDK v6.1.1 for enhanced features and performance
- **Real-time Streaming**: Low-latency video streaming over network
- **Pass-through Design**: Maintains original video quality while streaming
- **User-friendly Controls**: Easy-to-use parameters for configuration

## Requirements

- macOS 13+ (Ventura or later), Apple Silicon or Intel — or 64-bit Windows 10/11 (NVIDIA GPU for the GPU-native path; the CPU path is the automatic fallback)
- DaVinci Resolve 17+ (tested with DaVinci Resolve 20)
- Building from source additionally needs the NDI Advanced SDK 6.x — at `/Library/NDI Advanced SDK for Apple/` on macOS, `C:\Program Files\NDI\NDI 6 Advanced SDK` on Windows ([download](https://ndi.video/for-developers/)). The installers ship the NDI runtime inside the plugin, so end users need nothing else

## Installation

Grab the installer for your platform from the [latest release](https://github.com/lightsailvr/ResolveOFX_NDIOutput/releases/latest), then **restart DaVinci Resolve** — OFX plugins are only scanned at startup — and find the plugin on the Color page under **OpenFX → LSVR → NDIOutput**.

**macOS**: download `NDIOutput-<version>-macOS.pkg` and double-click it (signed and notarized; installs to `/Library/OFX/Plugins`).

**Windows** (from the first release whose assets include `NDIOutput-<version>-Windows-x64.exe` — the port is in the main tree but stays unreleased until it clears the release bar in [BUILD.md](BUILD.md)): download that installer and run it (quit Resolve first — the installer refuses to replace a loaded plugin; installs to `C:\Program Files\Common Files\OFX\Plugins`). x64 only, not Windows on ARM. Uninstall from **Add or Remove Programs**; `/VERYSILENT /SUPPRESSMSGBOXES /NORESTART` installs unattended for fleet deployment (it exits non-zero without installing if Resolve is running).

> **SmartScreen**: the Windows installer is **not code-signed**, so Windows shows *"Windows protected your PC"* the first time you run it. Click **More info**, then **Run anyway** — this is expected, not a malware verdict. To verify the download without relying on a signature, compare its SHA-256 against `SHA256SUMS-Windows.txt` on the release page: `Get-FileHash .\NDIOutput-<version>-Windows-x64.exe -Algorithm SHA256`.

The first NDI send raises a firewall prompt for Resolve on Windows — allow it on private networks, or the source is discoverable but its video is unreachable from other machines.

**From source**: full instructions (prerequisites, build, install, verification, troubleshooting, Windows status) live in **[BUILD.md](BUILD.md)**. The short version for macOS:

```bash
make dev            # build
sudo make install   # install to /Library/OFX/Plugins
```

## Usage

1. **Add the Effect**: In DaVinci Resolve, go to the Color page and add "NDI Output" from the LSVR category in the OpenFX panel.

2. **Configure Parameters**:
   - **NDI Source Name**: Set the name that will appear on the network (default: "DaVinci Resolve NDI Output")
   - **Enable NDI Output**: Toggle to start/stop streaming (default: enabled)
   - **Frame Rate**: Set the output frame rate (default: 30 fps)
   - **Resolution**: Stream at Full, Half, or Quarter of the incoming frame size (default: Half)

3. **HDR Configuration** (when working with HDR content):
   - **Enable HDR**: Toggle HDR mode for high dynamic range content
   - **Color Space**: Choose between Rec.709, Rec.2020, or DCI-P3
   - **Transfer Function**: Select SDR (Gamma 2.4), PQ (ST.2084), or HLG
   - **Max Content Light Level**: Set maximum brightness in nits (100-10000)
   - **Max Frame Average Light Level**: Set average brightness in nits (50-4000)

4. **Receive the Stream**: Use any NDI-compatible receiver (NDI Video Monitor, OBS Studio, etc.) to receive the stream on the network.

## Technical Details

### Architecture

- **Modern OFX C API**: Direct use of OpenFX C API for maximum host compatibility
- **NDI Advanced SDK Integration**: Leverages advanced NDI features for professional workflows
- **HDR Metadata**: Embeds proper HDR metadata for downstream applications
- **Efficient Processing**: Minimal overhead pass-through design

### HDR Implementation

The plugin supports comprehensive HDR workflows:

- **16-bit Processing**: HDR content uses 16-bit per channel for extended dynamic range
- **Metadata Embedding**: HDR parameters are embedded as XML metadata in NDI stream
- **Color Space Conversion**: Proper handling of different color spaces and transfer functions
- **Brightness Mapping**: Configurable mapping for different HDR standards

### Build System

The project uses a modern, streamlined build system:

- **Modern C API**: Direct use of OpenFX C API for maximum compatibility
- **NDI Advanced SDK**: Integration with NDI Advanced SDK v6.1.1
- **Automatic Versioning**: Semantic versioning with automatic patch increment
- **Clean Dependencies**: No legacy wrapper dependencies

## Development

- **Branching:** `master` is stable; all work happens on `dev` (or `feature/*` off `dev`) and merges to `master` by PR once validated. See [LEARNINGS.md](LEARNINGS.md) §1.
- **Building:** `make dev` / `make clean` — details in [BUILD.md](BUILD.md). No make target changes the version.
- **Versioning:** semantic versioning; bump only via `./scripts/increment_version.sh` (patch) or `./scripts/set_version.sh X.Y.Z`, which keep `VERSION` and the source `#define`s in sync.
- **Testing:** every change goes through the tiered testing loop in [LEARNINGS.md](LEARNINGS.md) §2 (compile → load in Resolve → verify stream in an NDI receiver → feature-specific checks) before it's considered done.

### Project Structure

```
├── src/
│   ├── NDIOutputPlugin.cpp          # Main plugin implementation (modern C API)
│   ├── NDIOutputPlugin.h            # Header file
│   └── LSVR.NDIOutput.png           # Plugin icon
├── scripts/
│   ├── increment_version.sh         # Auto-increment patch version
│   └── set_version.sh               # Manual version management
├── openfx/                          # OpenFX SDK
├── VERSION                          # Current semantic version
├── CHANGELOG.md                     # Version history
├── Makefile                         # Build configuration
├── Info.plist                       # Plugin metadata
└── README.md                        # This file
```

## Troubleshooting

### Plugin Not Loading

1. **Check NDI SDK Installation**:
   ```bash
   ls -la "/Library/NDI Advanced SDK for Apple/"
   ```

2. **Verify Plugin Installation**:
   ```bash
   ls -la "/Library/OFX/Plugins/NDIOutput.ofx.bundle"
   ```

3. **Check Library Dependencies**:
   ```bash
   otool -L "/Library/OFX/Plugins/NDIOutput.ofx.bundle/Contents/macOS/NDIOutput.ofx"
   ```

### No NDI Source Visible

1. **Check Plugin Parameters**: Ensure "Enable NDI Output" is checked
2. **Check Stream Status** (Stereo group): "No NDI sender — name unavailable" means the source name is already advertised on this machine (another app, or a leaked registration from a crashed session — `dns-sd -B _ndi._tcp local.` shows it). Change the NDI Source Name, or quit the process holding it
3. **Verify Network**: Ensure devices are on the same network
4. **Check NDI Tools**: Use NDI Video Monitor to verify source availability
5. **Restart DaVinci Resolve**: Sometimes required after parameter changes

### HDR Issues

1. **Check HDR Settings**: Verify color space and transfer function match your content
2. **Monitor Compatibility**: Ensure receiving device supports HDR metadata
3. **Content Verification**: Confirm source material is actually HDR

## Version History

The current version is in [`VERSION`](VERSION). Per-release details live in [CHANGELOG.md](CHANGELOG.md) and the `RELEASE_NOTES` files under [`Releases/`](Releases/). Notable fixes and the rules they taught us are logged in [LEARNINGS.md](LEARNINGS.md) §3.

## License

This project is licensed under the BSD 3-Clause License - see the LICENSE file for details.

## Contributing

1. Branch off `dev` (never work on `master`)
2. Make your changes; build with `make dev`
3. Validate through the testing loop in [LEARNINGS.md](LEARNINGS.md) §2
4. If you fixed a bug, add an entry to the log in [LEARNINGS.md](LEARNINGS.md) §3
5. Open a pull request into `master`

## Support

For issues and questions:
- Check the troubleshooting section above
- Review DaVinci Resolve console output for error messages
- Verify NDI SDK installation and network configuration

## Acknowledgments

NDI® is a registered trademark of Vizrt NDI AB — learn more at [ndi.video](https://ndi.video/). Receive the stream with the free [NDI Tools](https://ndi.video/tools/). Third-party license notices ship inside the plugin at `Contents/Resources/libndi_licenses.txt`.
