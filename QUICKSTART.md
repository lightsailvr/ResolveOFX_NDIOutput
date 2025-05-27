# Quick Start Guide

Get the NDI Output plugin running in DaVinci Resolve in just a few steps!

## Prerequisites (5 minutes)

### macOS
1. Install NDI SDK: Download from [ndi.video/for-developers](https://ndi.video/for-developers/)
2. Install Xcode Command Line Tools: `xcode-select --install`

### Windows
1. Install NDI SDK: Download from [ndi.video/for-developers](https://ndi.video/for-developers/)
2. Install Visual Studio 2019+ with C++ development tools

## Build & Install (2 minutes)

### macOS
```bash
git clone <this-repository>
cd NDIOutput_Standalone
./build/mac/build.sh
sudo make install
```

### Windows
```cmd
git clone <this-repository>
cd NDIOutput_Standalone
build\windows\build.bat install
```

## Use in DaVinci Resolve (1 minute)

1. **Restart DaVinci Resolve**
2. **Add Effect**: Drag "NDI Output" from Effects Library to your clip
3. **Configure**: Set NDI source name and enable output
4. **Receive**: Use any NDI app to receive the stream

## Test Your Setup

### Recommended NDI Receivers
- **NDI Studio Monitor** (Free from NDI Tools)
- **OBS Studio** (with NDI plugin)
- **VLC Media Player** (with NDI support)
- **Wirecast**
- **vMix**

### Quick Test
1. Download NDI Tools from [ndi.video/tools](https://ndi.video/tools/)
2. Install and run "NDI Studio Monitor"
3. Your DaVinci Resolve stream should appear in the source list

## Troubleshooting

**Plugin not appearing?**
- Restart DaVinci Resolve completely
- Check that NDI SDK is installed in the default location

**No NDI stream?**
- Verify NDI Tools can see other sources on your network
- Check firewall settings
- Ensure you've enabled the NDI output in the plugin

**Build failed?**
- macOS: Ensure NDI SDK is at `/Library/NDI SDK for Apple/`
- Windows: Use Visual Studio Developer Command Prompt

## Next Steps

- Read the full [README.md](README.md) for detailed information
- Check the troubleshooting section for common issues
- Explore advanced NDI applications for professional workflows

---

**Need help?** Create an issue with your system details and error messages. 