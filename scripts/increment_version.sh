#!/bin/bash

# Script to increment the patch version number
# Usage: ./scripts/increment_version.sh

VERSION_FILE="VERSION"

if [ ! -f "$VERSION_FILE" ]; then
    echo "1.0.0" > "$VERSION_FILE"
    echo "Created VERSION file with initial version 1.0.0"
fi

# Read current version
CURRENT_VERSION=$(cat "$VERSION_FILE")
echo "Current version: $CURRENT_VERSION"

# Parse version components
IFS='.' read -r MAJOR MINOR PATCH <<< "$CURRENT_VERSION"

# Increment patch version
PATCH=$((PATCH + 1))

# Create new version
NEW_VERSION="$MAJOR.$MINOR.$PATCH"

# Update VERSION file
echo "$NEW_VERSION" > "$VERSION_FILE"

# Update plugin source code
sed -i '' "s/#define kPluginVersionPatch [0-9]*/#define kPluginVersionPatch $PATCH/" src/NDIOutputPlugin.cpp
sed -i '' "s/#define kPluginVersionString \"[0-9]*\.[0-9]*\.[0-9]*\"/#define kPluginVersionString \"$NEW_VERSION\"/" src/NDIOutputPlugin.cpp

echo "Version incremented to: $NEW_VERSION"
echo "Updated files:"
echo "  - VERSION"
echo "  - src/NDIOutputPlugin.cpp"

# Show git status if in a git repository
if [ -d ".git" ]; then
    echo ""
    echo "Git status:"
    git status --porcelain VERSION src/NDIOutputPlugin.cpp
fi 