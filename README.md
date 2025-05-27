# NDI Advanced Output Plugin for DaVinci Resolve

A modern OpenFX plugin that sends video frames from DaVinci Resolve to NDI (Network Device Interface) for streaming over network with comprehensive HDR support.

> **Latest Release**: v1.1.4 - Fixed vertical flip issue, GPU acceleration framework ready  
> **Quick Install**: `cd Releases/v1.1.4/ && ./install.sh`

## Features

- **Modern OFX Implementation**: Uses C API directly for maximum compatibility with DaVinci Resolve 20+
- **HDR Support**: Full HDR workflow support with:
  - PQ (ST.2084) and HLG (Hybrid Log-Gamma) transfer functions
  - Rec.2020, DCI-P3, and Rec.709 color spaces
  - Configurable Max Content Light Level (CLL) and Max Frame Average Light Level (FALL)
  - HDR metadata embedding for proper downstream handling
- **NDI Advanced SDK**: Uses NDI Advanced SDK v6.1.1 for enhanced features and performance
- **Real-time Streaming**: Low-latency video streaming over network
- **Pass-through Design**: Maintains original video quality while streaming
- **User-friendly Controls**: Easy-to-use parameters for configuration

## Requirements

- macOS 10.15+ (Catalina or later)
- DaVinci Resolve 17+ (tested with DaVinci Resolve 20)
- NDI Advanced SDK v6.1.1 (automatically installed)

## Installation

### Quick Install from Release (Recommended)

1. **Download the latest release** from `Releases/v1.1.4/`
2. **Run the installer**:
   ```bash
   cd Releases/v1.1.4/
   ./install.sh
   ```
3. **Restart DaVinci Resolve**

### Prerequisites

1. **Install NDI Advanced SDK** (if not already installed):
   ```bash
   # The SDK should be installed at:
   /Library/NDI Advanced SDK for Apple/
   ```

2. **Create symbolic link** (required for build system):
   ```bash
   sudo ln -sf "/Library/NDI Advanced SDK for Apple" "/Library/NDI_Advanced_SDK"
   ```

### Development Build and Install

1. **Clone the repository**:
   ```bash
   git clone https://github.com/your-repo/LSVR_ResolveOFX_NDIOutput.git
   cd LSVR_ResolveOFX_NDIOutput
   ```

2. **Build and install**:
   ```bash
   make install
   ```

3. **Restart DaVinci Resolve** to load the new plugin.

## Usage

1. **Add the Effect**: In DaVinci Resolve, go to the Color page and add "NDI Output" from the LSVR category in the OpenFX panel.

2. **Configure Parameters**:
   - **NDI Source Name**: Set the name that will appear on the network (default: "DaVinci Resolve NDI Output")
   - **Enable NDI Output**: Toggle to start/stop streaming (default: enabled)
   - **Frame Rate**: Set the output frame rate (default: 25 fps)

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

### Building from Source

```bash
# Clean build
make clean

# Build with automatic version increment (production)
make

# Development build without version increment
make dev

# Install the plugin
make install
```

### Version Management

The project uses semantic versioning (MAJOR.MINOR.PATCH):

```bash
# Show current version
make show-version

# Automatic patch increment (done on 'make')
./scripts/increment_version.sh

# Manual version setting
./scripts/set_version.sh major 2.0.0    # Set major version
./scripts/set_version.sh minor 1.1.0    # Set minor version  
./scripts/set_version.sh patch 1.0.5    # Set patch version
./scripts/set_version.sh 1.2.3          # Set specific version

# Development builds (no version increment)
make dev
```

**Version Strategy**:
- **MAJOR**: Breaking changes or major feature releases
- **MINOR**: New features, backward compatible
- **PATCH**: Bug fixes, small improvements (auto-incremented on each build)

**Build Commands**:
- `make` - Production build with automatic patch increment
- `make dev` - Development build without version increment
- `make show-version` - Display current version

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
   ls -la "/Library/NDI_Advanced_SDK"  # Symbolic link
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
2. **Verify Network**: Ensure devices are on the same network
3. **Check NDI Tools**: Use NDI Video Monitor to verify source availability
4. **Restart DaVinci Resolve**: Sometimes required after parameter changes

### HDR Issues

1. **Check HDR Settings**: Verify color space and transfer function match your content
2. **Monitor Compatibility**: Ensure receiving device supports HDR metadata
3. **Content Verification**: Confirm source material is actually HDR

## Version History

### v1.0.x (Current)
- **v1.0.2**: Added semantic versioning system with automatic patch increment
- **v1.0.1**: Version system implementation
- **v1.0.0**: Modern OFX C API implementation with full HDR support
  - Modern OFX C API implementation
  - Full HDR support with PQ/HLG transfer functions
  - NDI Advanced SDK v6.1.1 integration
  - Improved compatibility with DaVinci Resolve 20+
  - Enhanced parameter controls and metadata handling

### v0.x (Legacy)
- Original implementation using OFX wrapper classes
- Basic NDI streaming functionality
- Limited HDR support

## License

This project is licensed under the BSD 3-Clause License - see the LICENSE file for details.

## Contributing

1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Test thoroughly with DaVinci Resolve
5. Submit a pull request

## Support

For issues and questions:
- Check the troubleshooting section above
- Review DaVinci Resolve console output for error messages
- Verify NDI SDK installation and network configuration
