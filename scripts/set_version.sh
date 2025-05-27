#!/bin/bash

# Script to manually set version numbers
# Usage: ./scripts/set_version.sh [major|minor|patch] [version]
# Examples:
#   ./scripts/set_version.sh major 2.0.0
#   ./scripts/set_version.sh minor 1.1.0
#   ./scripts/set_version.sh patch 1.0.5

VERSION_FILE="VERSION"

if [ $# -eq 0 ]; then
    echo "Usage: $0 [major|minor|patch] [version]"
    echo "       $0 [version]"
    echo ""
    echo "Examples:"
    echo "  $0 major 2.0.0    # Set major version"
    echo "  $0 minor 1.1.0    # Set minor version"
    echo "  $0 patch 1.0.5    # Set patch version"
    echo "  $0 1.2.3          # Set specific version"
    echo ""
    echo "Current version: $(cat $VERSION_FILE 2>/dev/null || echo 'not set')"
    exit 1
fi

# Read current version
CURRENT_VERSION=$(cat "$VERSION_FILE" 2>/dev/null || echo "1.0.0")
echo "Current version: $CURRENT_VERSION"

# Parse current version components
IFS='.' read -r CURRENT_MAJOR CURRENT_MINOR CURRENT_PATCH <<< "$CURRENT_VERSION"

if [ $# -eq 1 ]; then
    # Direct version setting
    NEW_VERSION="$1"
elif [ $# -eq 2 ]; then
    # Type-based version setting
    TYPE="$1"
    NEW_VERSION="$2"
    
    case "$TYPE" in
        major)
            # Validate major version format (X.0.0)
            if [[ ! "$NEW_VERSION" =~ ^[0-9]+\.0\.0$ ]]; then
                echo "Error: Major version should be in format X.0.0 (e.g., 2.0.0)"
                exit 1
            fi
            ;;
        minor)
            # Validate minor version format (X.Y.0)
            if [[ ! "$NEW_VERSION" =~ ^[0-9]+\.[0-9]+\.0$ ]]; then
                echo "Error: Minor version should be in format X.Y.0 (e.g., 1.1.0)"
                exit 1
            fi
            ;;
        patch)
            # Validate patch version format (X.Y.Z)
            if [[ ! "$NEW_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
                echo "Error: Patch version should be in format X.Y.Z (e.g., 1.0.5)"
                exit 1
            fi
            ;;
        *)
            echo "Error: Invalid type '$TYPE'. Use 'major', 'minor', or 'patch'"
            exit 1
            ;;
    esac
else
    echo "Error: Invalid number of arguments"
    exit 1
fi

# Validate version format
if [[ ! "$NEW_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "Error: Version must be in format X.Y.Z (e.g., 1.2.3)"
    exit 1
fi

# Parse new version components
IFS='.' read -r NEW_MAJOR NEW_MINOR NEW_PATCH <<< "$NEW_VERSION"

# Update VERSION file
echo "$NEW_VERSION" > "$VERSION_FILE"

# Update plugin source code
sed -i '' "s/#define kPluginVersionMajor [0-9]*/#define kPluginVersionMajor $NEW_MAJOR/" src/NDIOutputPlugin.cpp
sed -i '' "s/#define kPluginVersionMinor [0-9]*/#define kPluginVersionMinor $NEW_MINOR/" src/NDIOutputPlugin.cpp
sed -i '' "s/#define kPluginVersionPatch [0-9]*/#define kPluginVersionPatch $NEW_PATCH/" src/NDIOutputPlugin.cpp
sed -i '' "s/#define kPluginVersionString \"[0-9]*\.[0-9]*\.[0-9]*\"/#define kPluginVersionString \"$NEW_VERSION\"/" src/NDIOutputPlugin.cpp

echo "Version updated to: $NEW_VERSION"
echo "Updated files:"
echo "  - VERSION"
echo "  - src/NDIOutputPlugin.cpp"

# Show git status if in a git repository
if [ -d ".git" ]; then
    echo ""
    echo "Git status:"
    git status --porcelain VERSION src/NDIOutputPlugin.cpp
fi 