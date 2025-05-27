#!/bin/bash

# NDI Output Plugin v1.1.4 Installation Script
# For DaVinci Resolve on macOS

echo "🚀 NDI Output Plugin v1.1.4 Installer"
echo "======================================"

# Check if running on macOS
if [[ "$OSTYPE" != "darwin"* ]]; then
    echo "❌ Error: This installer is for macOS only"
    exit 1
fi

# Check if NDI Advanced SDK is installed
if [ ! -d "/Library/NDI_Advanced_SDK" ]; then
    echo "⚠️  Warning: NDI Advanced SDK not found at /Library/NDI_Advanced_SDK"
    echo "   Please install NDI Advanced SDK before using this plugin"
    echo "   Download from: https://ndi.video/sdk/"
    echo ""
fi

# Check if OFX Plugins directory exists
OFX_DIR="/Library/OFX/Plugins"
if [ ! -d "$OFX_DIR" ]; then
    echo "📁 Creating OFX Plugins directory..."
    sudo mkdir -p "$OFX_DIR"
fi

# Remove old version if exists
if [ -d "$OFX_DIR/NDIOutput.ofx.bundle" ]; then
    echo "🗑️  Removing previous version..."
    sudo rm -rf "$OFX_DIR/NDIOutput.ofx.bundle"
fi

# Install new version
echo "📦 Installing NDI Output Plugin v1.1.4..."
sudo cp -R "macOS/NDIOutput.ofx.bundle" "$OFX_DIR/"

# Fix library paths
echo "🔧 Fixing library paths..."
sudo install_name_tool -change "@rpath/libndi_advanced.dylib" "/Library/NDI_Advanced_SDK/lib/macOS/libndi_advanced.dylib" "$OFX_DIR/NDIOutput.ofx.bundle/Contents/macOS/NDIOutput.ofx"

# Set proper permissions
echo "🔐 Setting permissions..."
sudo chown -R root:wheel "$OFX_DIR/NDIOutput.ofx.bundle"
sudo chmod -R 755 "$OFX_DIR/NDIOutput.ofx.bundle"

echo ""
echo "✅ Installation completed successfully!"
echo ""
echo "📋 Next Steps:"
echo "   1. Restart DaVinci Resolve"
echo "   2. Look for 'NDI Output' in Effects Library under 'LSVR'"
echo "   3. Add the effect to your timeline"
echo "   4. Configure NDI source name and settings"
echo ""
echo "🎮 New Features in v1.1.4:"
echo "   • Fixed vertical flip issue"
echo "   • GPU acceleration framework"
echo "   • Asynchronous processing"
echo "   • UYVY format support"
echo "   • Enhanced HDR support"
echo ""
echo "📞 Support: Check console output for debugging information"
echo "" 