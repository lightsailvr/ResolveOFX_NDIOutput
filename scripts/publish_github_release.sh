#!/bin/bash

# Create a GitHub release (as a DRAFT unless --publish) for the current VERSION,
# attaching the signed installer artifacts produced by scripts/package_release.sh.
#
# Usage: ./scripts/publish_github_release.sh [--publish] [--notes-file <file>]
#
# Refuses to upload anything unsigned or un-notarized: the pkg must pass
# `xcrun stapler validate` first. Release notes come from the CHANGELOG.md
# section matching the version (## [X.Y.Z]) unless --notes-file is given.
#
# One release event, per-platform artifacts (ticket #23): if the Windows
# artifacts built by scripts/package_windows_release.ps1 on a Windows machine
# have been copied into the same dist/v<VERSION>/ directory, they are attached
# alongside the pkg and the notes grow a Windows install section. Without them
# the release is macOS-only and says so.

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

# Windows artifacts are optional (they are built on a Windows machine and
# copied in), but all-or-nothing: a half-attached platform is worse than none.
WIN_EXE="$DIST/NDIOutput-$VERSION-Windows-x64.exe"
WIN_ZIP="$DIST/NDIOutput-$VERSION-Windows-x64.zip"
WIN_SUMS="$DIST/SHA256SUMS-Windows.txt"
WIN_FILES=()
win_present=0
# A STUB build is the CI pipeline proof — it loads but never streams. Checked
# before the count so its presence reports the real problem, not "1 of 3".
if ls "$DIST"/*-STUB.* >/dev/null 2>&1; then
    echo "STUB artifacts present in $DIST — those never ship. Remove them and package against the real NDI SDK."
    exit 1
fi
for f in "$WIN_EXE" "$WIN_ZIP" "$WIN_SUMS"; do
    [ -f "$f" ] && win_present=$((win_present + 1))
done
if [ "$win_present" -eq 3 ]; then
    WIN_FILES=("$WIN_EXE" "$WIN_ZIP" "$WIN_SUMS")
    echo "Windows artifacts found — this will be a macOS + Windows release."
elif [ "$win_present" -eq 0 ]; then
    echo "No Windows artifacts in $DIST — releasing macOS only."
else
    echo "Only $win_present of 3 Windows artifacts present in $DIST:"
    for f in "$WIN_EXE" "$WIN_ZIP" "$WIN_SUMS"; do
        [ -f "$f" ] || echo "  missing: $f"
    done
    echo "Copy all three from the Windows machine (scripts/package_windows_release.ps1), or remove them."
    exit 1
fi

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

### Install — macOS

Download `NDIOutput-*-macOS.pkg`, double-click, done. Restart DaVinci Resolve, then find the plugin on the Color page under **OpenFX → LSVR → NDIOutput**. Requires macOS 13+ (Apple Silicon or Intel); the NDI runtime is included.

The `.zip` contains the bare signed plugin bundle for manual installs into `/Library/OFX/Plugins`.
EOF

if [ "$win_present" -eq 3 ]; then
    cat >> "$NOTES" <<'EOF'

### Install — Windows

Download `NDIOutput-*-Windows-x64.exe` and run it (quit DaVinci Resolve first — the installer refuses to replace a loaded plugin). Restart Resolve, then find the plugin on the Color page under **OpenFX → LSVR → NDIOutput**. Requires 64-bit **x64** Windows 10/11 — **Windows on ARM is not supported**. The NDI runtime is included.

The GPU-native path needs an NVIDIA GPU; on other hardware the plugin falls back to the CPU path automatically, which carries a lower practical resolution ceiling (use the Resolution control to stream Half or Quarter).

**This installer is not code-signed**, so Windows SmartScreen shows *"Windows protected your PC"*. Click **More info → Run anyway**. To verify the download instead of trusting a signature, compare its SHA-256 against `SHA256SUMS-Windows.txt`:

```powershell
Get-FileHash .\NDIOutput-<version>-Windows-x64.exe -Algorithm SHA256
```

Silent install for fleet deployment: `NDIOutput-<version>-Windows-x64.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART`. Push it to machines with Resolve closed — with Resolve running the installer exits non-zero and installs nothing rather than touching a loaded plugin. Uninstall from **Add or Remove Programs**.

The `.zip` contains the bare plugin bundle for manual installs into `C:\Program Files\Common Files\OFX\Plugins`.

The first NDI send raises a Windows Firewall prompt for Resolve — allow it on private networks, or the stream stays unreachable from other machines.
EOF
fi

cat >> "$NOTES" <<'EOF'

---

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
    "$PKG" "$ZIP" "$SUMS" ${WIN_FILES[@]+"${WIN_FILES[@]}"}

echo ""
if [ "$PUBLISH" -eq 1 ]; then
    echo "Release $TAG is LIVE."
else
    echo "Draft release $TAG created — review it on GitHub, then publish with:"
    echo "  gh release edit $TAG --draft=false"
fi
