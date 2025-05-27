#!/bin/bash

# NDI Output Plugin Build Script for macOS
# This script builds the NDI Output plugin for DaVinci Resolve with HDR framework

set -e  # Exit on any error

echo "=== NDI Output Plugin Build Script ==="
echo "Building for macOS with NDI 6.1+ SDK support..."

# Check if NDI SDK is installed (either standard or advanced)
NDI_SDK_PATH="/Library/NDI SDK for Apple"
NDI_ADVANCED_PATH="/Library/NDI Advanced SDK for Apple"

if [ -d "$NDI_ADVANCED_PATH" ]; then
    echo "✓ NDI Advanced SDK found at $NDI_ADVANCED_PATH"
    echo "  Full HDR features will be available"
elif [ -d "$NDI_SDK_PATH" ]; then
    echo "✓ NDI SDK found at $NDI_SDK_PATH"
    # Check version
    if [ -f "$NDI_SDK_PATH/Version.txt" ]; then
        VERSION=$(cat "$NDI_SDK_PATH/Version.txt")
        echo "  Version: $VERSION"
        if [[ "$VERSION" == *"v6."* ]]; then
            echo "  NDI 6.x detected - HDR framework included"
        else
            echo "  Note: For full HDR support, consider upgrading to NDI Advanced SDK"
        fi
    fi
else
    echo "ERROR: NDI SDK not found"
    echo "Please install either:"
    echo "  - NDI SDK for Apple (standard)"
    echo "  - NDI Advanced SDK for Apple (full HDR support)"
    echo "Download from: https://ndi.video/for-developers/"
    exit 1
fi

# Check for Xcode command line tools
if ! command -v clang &> /dev/null; then
    echo "ERROR: Xcode command line tools not found"
    echo "Please install with: xcode-select --install"
    exit 1
fi

echo "✓ Xcode command line tools found"

# Navigate to project root
cd "$(dirname "$0")/../.."

# Clean previous builds
echo "Cleaning previous builds..."
make clean 2>/dev/null || true

# Build the plugin
echo "Building NDI Output plugin..."
make -j$(sysctl -n hw.ncpu)

if [ $? -eq 0 ]; then
    echo "✓ Build completed successfully!"
    
    # Create release package
    echo "Creating release package..."
    RELEASE_DIR="Releases/v2.0-HDR/macOS"
    mkdir -p "$RELEASE_DIR"
    
    # Copy the built plugin
    if [ -d "NDIOutput.ofx.bundle" ]; then
        cp -R "NDIOutput.ofx.bundle" "$RELEASE_DIR/"
        echo "✓ Plugin copied to $RELEASE_DIR/"
    else
        echo "Warning: Plugin bundle not found in expected location"
        # Look for it in other common locations
        find . -name "NDIOutput.ofx.bundle" -type d 2>/dev/null | head -1 | while read bundle; do
            if [ -n "$bundle" ]; then
                cp -R "$bundle" "$RELEASE_DIR/"
                echo "✓ Plugin found and copied from $bundle to $RELEASE_DIR/"
            fi
        done
    fi
    
    # Check if installation was requested
    if [ "$1" = "install" ]; then
        echo "Installing plugin..."
        sudo make install
        
        if [ $? -eq 0 ]; then
            echo "✓ Plugin installed successfully!"
            echo "Plugin location: /Library/OFX/Plugins/NDIOutput.ofx.bundle/"
            echo ""
            echo "Next steps:"
            echo "1. Restart DaVinci Resolve"
            echo "2. Look for 'NDI Output' in the OpenFX effects"
            echo "3. Configure HDR settings as needed"
        else
            echo "✗ Installation failed"
            exit 1
        fi
    else
        echo ""
        echo "Build completed! Plugin available in: $RELEASE_DIR/"
        echo ""
        echo "To install manually:"
        echo "  sudo cp -R '$RELEASE_DIR/NDIOutput.ofx.bundle' '/Library/OFX/Plugins/'"
        echo ""
        echo "Or run this script with 'install' parameter:"
        echo "  ./build/mac/build.sh install"
    fi
else
    echo "✗ Build failed"
    exit 1
fi

echo ""
echo "=== Build Complete ==="
