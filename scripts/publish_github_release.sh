#!/bin/bash

# Create a GitHub release (as a DRAFT unless --publish) for the current VERSION,
# attaching the signed installer artifacts produced by scripts/package_release.sh.
#
# Usage: ./scripts/publish_github_release.sh [--publish] [--notes-file <file>]
#
# Refuses to upload anything unsigned or un-notarized: the pkg must pass
# `xcrun stapler validate` first. Release notes come from the CHANGELOG.md
# section matching the version (## [X.Y.Z]) unless --notes-file is given.

set -euo pipefail

cd "$(dirname "$0")/.."

VERSION=$(cat VERSION)
TAG="v$VERSION"
DIST="dist/v$VERSION"
PKG="$DIST/NDIOutput-$VERSION-macOS.pkg"
ZIP="$DIST/NDIOutput-$VERSION-macOS.zip"
SUMS="$DIST/SHA256SUMS.txt"

PUBLISH=0
NOTES_FILE=""
while [ $# -gt 0 ]; do
    case "$1" in
        --publish) PUBLISH=1 ;;
        --notes-file) NOTES_FILE="$2"; shift ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
    shift
done

# ------------------------------------------------------------- preflight ----
for f in "$PKG" "$ZIP" "$SUMS"; do
    [ -f "$f" ] || { echo "Missing $f — run ./scripts/package_release.sh first."; exit 1; }
done

echo "Validating notarization staple on $PKG..."
xcrun stapler validate "$PKG" || {
    echo "Pkg is not stapled/notarized — never release an unsigned build."; exit 1; }
spctl -a -vv -t install "$PKG" 2>&1 | grep -q "accepted" || {
    echo "Gatekeeper does not accept this pkg."; exit 1; }

gh auth status >/dev/null 2>&1 || { echo "gh not authenticated — run: gh auth login"; exit 1; }

branch=$(git branch --show-current)
if [ "$branch" != "master" ]; then
    echo "WARNING: releasing from branch '$branch', not master."
fi

# ---------------------------------------------------------- release notes ----
NOTES="$DIST/release_notes.md"
if [ -n "$NOTES_FILE" ]; then
    cp "$NOTES_FILE" "$NOTES"
else
    # Extract the "## [X.Y.Z]" section from CHANGELOG.md
    awk -v ver="$VERSION" '
        $0 ~ "^## \\[" ver "\\]" {grab=1; next}
        grab && /^## \[/ {exit}
        grab {print}' CHANGELOG.md > "$NOTES"
    if [ ! -s "$NOTES" ]; then
        echo "No CHANGELOG.md section found for $VERSION."
        echo "Add a '## [$VERSION]' entry (or pass --notes-file)."
        exit 1
    fi
fi

cat >> "$NOTES" <<'EOF'

---

### Install

Download `NDIOutput-*-macOS.pkg`, double-click, done. Restart DaVinci Resolve, then find the plugin on the Color page under **OpenFX → LSVR → NDIOutput**. Requires macOS 13+ (Apple Silicon or Intel); the NDI runtime is included.

The `.zip` contains the bare signed plugin bundle for manual installs into `/Library/OFX/Plugins`.

NDI® is a registered trademark of Vizrt NDI AB — [ndi.video](https://ndi.video/)
EOF

# ------------------------------------------------------------ tag + release ----
if ! git rev-parse "$TAG" >/dev/null 2>&1; then
    echo "Creating tag $TAG at HEAD..."
    git tag -a "$TAG" -m "Release $TAG"
    git push origin "$TAG"
else
    echo "Tag $TAG already exists."
fi

DRAFT_FLAG="--draft"
[ "$PUBLISH" -eq 1 ] && DRAFT_FLAG=""

echo "Creating GitHub release $TAG${DRAFT_FLAG:+ (draft)}..."
gh release create "$TAG" $DRAFT_FLAG \
    --title "$TAG" \
    --notes-file "$NOTES" \
    "$PKG" "$ZIP" "$SUMS"

echo ""
if [ "$PUBLISH" -eq 1 ]; then
    echo "Release $TAG is LIVE."
else
    echo "Draft release $TAG created — review it on GitHub, then publish with:"
    echo "  gh release edit $TAG --draft=false"
fi
