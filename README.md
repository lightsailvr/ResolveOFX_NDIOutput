# NDI Output Plugin for DaVinci Resolve

A professional OFX (OpenFX) plugin that enables real-time NDI (Network Device Interface) streaming directly from DaVinci Resolve's timeline. Stream your video content over the network to any NDI-compatible application or device.

![NDI Output Plugin](src/BaldavengerOFX.NDIOutput.png)

## Features

- **Real-time NDI Streaming**: Send video frames from DaVinci Resolve to NDI network
- **Pass-through Effect**: Doesn't modify your video - acts as a transparent output
- **Configurable Parameters**:
  - NDI Source Name: Set custom name visible on network
  - Enable/Disable: Toggle NDI output on/off
  - Frame Rate Configuration: Match your project settings
- **Professional Quality**: Float RGBA processing with proper format conversion
- **Cross-platform**: Supports both macOS and Windows

## System Requirements

### macOS
- macOS 10.14 or later
- DaVinci Resolve 17+
- NDI SDK for Apple
- Xcode Command Line Tools

### Windows
- Windows 10 64-bit or later
- DaVinci Resolve 17+
- NDI SDK for Windows
- Visual Studio 2019 or later with C++ development tools

## Prerequisites

### 1. Install NDI SDK

#### macOS
1. Download NDI SDK for Apple from [NDI Developer Portal](https://ndi.video/for-developers/)
2. Install to default location: `/Library/NDI SDK for Apple/`

#### Windows
1. Download NDI SDK for Windows from [NDI Developer Portal](https://ndi.video/for-developers/)
2. Install to default location: `C:\Program Files\NDI\NDI 5 SDK\`

### 2. Development Tools

#### macOS
```bash
# Install Xcode Command Line Tools
xcode-select --install
```

#### Windows
1. Install Visual Studio 2019 or later
2. Include "Desktop development with C++" workload
3. Ensure Windows 10 SDK is installed

## Building the Plugin

### macOS Build

1. **Clone and navigate to the repository**:
   ```bash
   git clone <repository-url>
   cd NDIOutput_Standalone
   ```

2. **Run the build script**:
   ```bash
   ./build/mac/build.sh
   ```

3. **Or build manually**:
   ```bash
   make clean
   make
   ```

4. **Install the plugin**:
   ```bash
   sudo make install
   ```

### Windows Build

1. **Open Developer Command Prompt**:
   - Launch "Developer Command Prompt for VS 2019" (or your VS version)

2. **Navigate to the project**:
   ```cmd
   cd path\to\NDIOutput_Standalone
   ```

3. **Run the build script**:
   ```cmd
   build\windows\build.bat
   ```

4. **Build and install in one step**:
   ```cmd
   build\windows\build.bat install
   ```

### Manual Windows Build

If you prefer to build manually:

```cmd
cd build\windows

# Compile
cl /I"..\..\openfx\include" /I"C:\Program Files\NDI\NDI 5 SDK\Include" /std:c++11 /EHsc /c "..\..\src\NDIOutputPlugin.cpp"

# Link
link NDIOutputPlugin.obj "C:\Program Files\NDI\NDI 5 SDK\Lib\x64\Processing.NDI.Lib.x64.lib" /OUT:NDIOutput.ofx /DLL /SUBSYSTEM:WINDOWS
```

## Installation

### Automatic Installation

#### macOS
```bash
sudo make install
```

#### Windows
```cmd
build\windows\build.bat install
```

### Manual Installation

#### macOS
Copy the plugin bundle to:
```
/Library/OFX/Plugins/NDIOutput.ofx.bundle/
```

#### Windows
Create the following directory structure:
```
C:\Program Files\Common Files\OFX\Plugins\NDIOutput.ofx.bundle\
├── Contents\
│   ├── Info.plist
│   └── Win64\
│       └── NDIOutput.ofx
```

## Usage in DaVinci Resolve

1. **Add the Effect**:
   - Open DaVinci Resolve
   - Navigate to Color or Edit page
   - Find "NDI Output" in Effects Library under "OpenFX"
   - Drag onto your clip or timeline

2. **Configure Settings**:
   - Set NDI Source Name (appears on network)
   - Enable NDI Output
   - Adjust frame rate if needed

3. **Receive the Stream**:
   - Use any NDI-compatible application
   - Look for your source name on the network
   - Connect and receive the stream

## Troubleshooting

### Build Issues

#### macOS
- **NDI SDK not found**: Ensure installed at `/Library/NDI SDK for Apple/`
- **Command line tools missing**: Run `xcode-select --install`
- **Permission denied**: Use `sudo` for installation

#### Windows
- **'cl' not recognized**: Use Visual Studio Developer Command Prompt
- **NDI headers not found**: Verify NDI SDK installation path
- **Link errors**: Ensure 64-bit NDI libraries are available

### Runtime Issues

#### Plugin Not Appearing
- Restart DaVinci Resolve completely
- Check plugin installation path
- Verify OFX plugin directory permissions

#### NDI Not Working
- Install NDI Tools for testing
- Check firewall settings (allow NDI traffic)
- Verify network connectivity
- Ensure NDI Runtime is installed

### Performance Tips

- Use wired network for best performance
- Match frame rates between source and receiver
- Consider network bandwidth limitations
- Monitor CPU usage during streaming

## Development

### Project Structure
```
NDIOutput_Standalone/
├── src/                          # Source code
│   ├── NDIOutputPlugin.cpp       # Main plugin implementation
│   ├── NDIOutputPlugin.h         # Plugin header
│   ├── libndi.dylib             # NDI library (macOS)
│   └── BaldavengerOFX.NDIOutput.png # Plugin icon
├── openfx/                       # OpenFX framework
│   ├── include/                  # OFX headers
│   └── Support/                  # OFX support library
├── SupportExt/                   # Extended support utilities
├── build/                        # Build scripts
│   ├── mac/build.sh             # macOS build script
│   └── windows/                 # Windows build files
├── Makefile                      # Main build configuration
└── README.md                     # This file
```

### Building from Source

The plugin uses the OpenFX standard and includes all necessary dependencies. The build system automatically handles:

- OpenFX framework integration
- NDI SDK linking
- Platform-specific compilation
- Plugin bundle creation

### Contributing

1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Test on both platforms
5. Submit a pull request

## Technical Details

- **Plugin Type**: OFX ImageEffect Filter
- **Pixel Format**: Float RGBA → uint8 RGBA conversion
- **Threading**: Thread-safe rendering
- **NDI Format**: Progressive RGBA frames
- **Timecode**: Synthesized by NDI SDK

## License

This project is licensed under the GPL v3 License - see the [LICENSE](LICENSE) file for details.

## Acknowledgments

- Based on the BaldavengerOFX plugin collection
- Uses the OpenFX standard for plugin development
- Integrates NDI SDK from Vizrt/NewTek
- Inspired by the professional broadcast industry's need for IP-based workflows

## Support

For issues and questions:
1. Check the troubleshooting section above
2. Verify your NDI SDK installation
3. Test with NDI Tools first
4. Create an issue with detailed system information

## Version History

- **v1.0**: Initial release with basic NDI streaming functionality
- Cross-platform support for macOS and Windows
- Real-time video streaming with configurable parameters
