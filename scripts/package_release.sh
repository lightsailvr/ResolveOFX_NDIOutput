#!/bin/bash

# Build a signed, notarized macOS installer (.pkg) for the NDI Output plugin,
# plus a signed .zip of the bare bundle for manual installs.
#
# Usage: ./scripts/package_release.sh [--skip-notarize] [--skip-tests] [--host-arch-only]
#
# Output lands in dist/v<VERSION>/:
#   NDIOutput-<VERSION>-macOS.pkg   signed + notarized + stapled installer
#   NDIOutput-<VERSION>-macOS.zip   signed bundle (manual install into /Library/OFX/Plugins)
#   SHA256SUMS.txt
#
# What this does differently from `make install` (the dev flow):
#   - universal binary (arm64 + x86_64), macOS 13.0 deployment target
#   - bundles libndi_advanced.dylib inside the bundle (Contents/Frameworks/)
#     and rewrites the link reference to @loader_path — end users do NOT need
#     the NDI SDK installed
#   - ships the NDI third-party license file (libndi_licenses.txt) in
#     Contents/Resources/ per the NDI SDK distribution requirements
#   - codesigns everything with the Developer ID Application certificate,
#     signs the pkg with the Developer ID Installer certificate, then
#     notarizes and staples
#
# One-time setup (see BUILD.md "Release packaging"):
#   1. Developer ID Application cert in the login keychain   (present already)
#   2. Developer ID Installer cert in the login keychain     (create in Xcode:
#      Settings -> Accounts -> Manage Certificates -> + -> Developer ID Installer)
#   3. Notarization credentials stored as a keychain profile:
#      xcrun notarytool store-credentials "$NOTARY_PROFILE" \
#          --apple-id <your-apple-id> --team-id <TEAMID>
#      (prompts for an app-specific password from account.apple.com)

set -euo pipefail

cd "$(dirname "$0")/.."

# ---------------------------------------------------------------- config ----
NDI_SDK_PATH="/Library/NDI Advanced SDK for Apple"
NDI_DYLIB="$NDI_SDK_PATH/lib/macOS/libndi_advanced.dylib"
NDI_LICENSES="$NDI_SDK_PATH/licenses/libndi_licenses.txt"
PKG_ID="com.lightsailvr.ndioutput"
NOTARY_PROFILE="${NOTARY_PROFILE:-NDI_NOTARY}"
DEPLOYMENT_TARGET="13.0"

SKIP_NOTARIZE=0
SKIP_TESTS=0
UNSIGNED_DEV=0
ARCHFLAGS="-arch arm64 -arch x86_64"
for arg in "$@"; do
    case "$arg" in
        --skip-notarize)  SKIP_NOTARIZE=1 ;;
        --skip-tests)     SKIP_TESTS=1 ;;
        --host-arch-only) ARCHFLAGS="" ;;
        # Pipeline smoke test without the Installer cert / notary profile:
        # produces an UNSIGNED pkg that must never be distributed.
        --unsigned-dev-build) UNSIGNED_DEV=1; SKIP_NOTARIZE=1 ;;
        *) echo "Unknown option: $arg"; exit 1 ;;
    esac
done

VERSION=$(cat VERSION)
DIST="dist/v$VERSION"
STAGE="$DIST/stage"          # pkg root (contains the bundle)
BUNDLE="$STAGE/NDIOutput.ofx.bundle"
PKG_OUT="$DIST/NDIOutput-$VERSION-macOS.pkg"
ZIP_OUT="$DIST/NDIOutput-$VERSION-macOS.zip"

# ------------------------------------------------------------- preflight ----
fail=0
note() { echo "  [ok] $1"; }
miss() { echo "  [MISSING] $1"; fail=1; }

echo "Preflight for v$VERSION:"

[ -f "$NDI_DYLIB" ] && note "NDI Advanced SDK dylib" || miss "NDI Advanced SDK at $NDI_SDK_PATH"
[ -f "$NDI_LICENSES" ] && note "libndi_licenses.txt" || miss "$NDI_LICENSES"

# Version defines must match the VERSION file (the set_version.sh contract)
src_ver=$(awk '/#define kPluginVersionMajor/{maj=$3} /#define kPluginVersionMinor/{min=$3} /#define kPluginVersionPatch/{pat=$3} END{print maj"."min"."pat}' src/NDIOutputPlugin.cpp)
if [ "$src_ver" = "$VERSION" ]; then
    note "VERSION file matches source defines ($VERSION)"
else
    miss "VERSION ($VERSION) != src defines ($src_ver) — run ./scripts/set_version.sh"
fi

APP_CERT=$(security find-identity -v -p codesigning 2>/dev/null | grep -o '"Developer ID Application: [^"]*"' | head -1 | tr -d '"') || true
if [ -n "${APP_CERT:-}" ]; then
    note "signing cert: $APP_CERT"
else
    miss "Developer ID Application certificate (create at developer.apple.com or in Xcode)"
fi

INSTALLER_CERT=$(security find-identity -v 2>/dev/null | grep -o '"Developer ID Installer: [^"]*"' | head -1 | tr -d '"') || true
if [ -n "${INSTALLER_CERT:-}" ]; then
    note "installer cert: $INSTALLER_CERT"
elif [ "$UNSIGNED_DEV" -eq 1 ]; then
    echo "  [warn] no Developer ID Installer cert — pkg will be UNSIGNED (dev build)"
else
    miss "Developer ID Installer certificate (Xcode -> Settings -> Accounts -> Manage Certificates -> + -> Developer ID Installer)"
fi

if [ "$SKIP_NOTARIZE" -eq 0 ]; then
    if xcrun notarytool history --keychain-profile "$NOTARY_PROFILE" >/dev/null 2>&1; then
        note "notarytool keychain profile '$NOTARY_PROFILE'"
    else
        miss "notarytool profile '$NOTARY_PROFILE' — run: xcrun notarytool store-credentials $NOTARY_PROFILE --apple-id <apple-id> --team-id <TEAMID>"
    fi
fi

if [ "$fail" -ne 0 ]; then
    echo ""
    echo "Preflight failed — fix the [MISSING] items above (see BUILD.md 'Release packaging')."
    exit 1
fi

# ----------------------------------------------------------------- build ----
echo ""
echo "Building v$VERSION (universal: ${ARCHFLAGS:-host arch only}, macOS $DEPLOYMENT_TARGET+)..."
make clean >/dev/null
make dev DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" ARCHFLAGS="$ARCHFLAGS" >/dev/null
echo "  built: $(lipo -info NDIOutput.ofx.bundle/Contents/MacOS/NDIOutput.ofx | sed 's/.*are: //;s/Non-fat file.*architecture: //')"

if [ "$SKIP_TESTS" -eq 0 ]; then
    echo "Running unit tests..."
    make test >/dev/null
    echo "  unit tests pass"
fi

# ----------------------------------------------------------------- stage ----
echo "Staging bundle..."
rm -rf "$DIST"
mkdir -p "$STAGE"
cp -R NDIOutput.ofx.bundle "$BUNDLE"

# Bundle the NDI dylib and point the plugin at it relative to itself
mkdir -p "$BUNDLE/Contents/Frameworks"
cp "$NDI_DYLIB" "$BUNDLE/Contents/Frameworks/"
install_name_tool -change "@rpath/libndi_advanced.dylib" \
    "@loader_path/../Frameworks/libndi_advanced.dylib" \
    "$BUNDLE/Contents/MacOS/NDIOutput.ofx"

# NDI license attribution ships inside the bundle
cp "$NDI_LICENSES" "$BUNDLE/Contents/Resources/"

# Sync Info.plist versions to the real plugin version
/usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString $VERSION" "$BUNDLE/Contents/Info.plist"
/usr/libexec/PlistBuddy -c "Set :CFBundleVersion $VERSION" "$BUNDLE/Contents/Info.plist"

# ------------------------------------------------------------------ sign ----
echo "Codesigning (inside-out)..."
codesign --force --timestamp --options runtime --sign "$APP_CERT" \
    "$BUNDLE/Contents/Frameworks/libndi_advanced.dylib"
codesign --force --timestamp --options runtime --sign "$APP_CERT" \
    --identifier "LSVR.NDIOutput" "$BUNDLE"
codesign --verify --strict --deep --verbose=1 "$BUNDLE"
echo "  bundle signed and verified"

# ------------------------------------------------------------------- pkg ----
echo "Building installer pkg..."
COMPONENT_PLIST="$DIST/component.plist"
pkgbuild --analyze --root "$STAGE" "$COMPONENT_PLIST" >/dev/null
# The bundle must always land in /Library/OFX/Plugins — never "relocate" to
# wherever Spotlight last saw a copy (e.g. a dev checkout).
/usr/libexec/PlistBuddy -c "Set :0:BundleIsRelocatable false" "$COMPONENT_PLIST" 2>/dev/null || \
    /usr/libexec/PlistBuddy -c "Add :0:BundleIsRelocatable bool false" "$COMPONENT_PLIST"
# ...and always replace whatever is there. With the default version check,
# PackageKit compares CFBundleShortVersionString and silently SKIPS the payload
# (while still writing the receipt!) if the installed bundle claims a higher
# version — which dev installs did for years via the old placeholder "2.0"
# Info.plist. See LEARNINGS 2026-08-30.
/usr/libexec/PlistBuddy -c "Set :0:BundleIsVersionChecked false" "$COMPONENT_PLIST" 2>/dev/null || \
    /usr/libexec/PlistBuddy -c "Add :0:BundleIsVersionChecked bool false" "$COMPONENT_PLIST"

COMPONENT_PKG="$DIST/NDIOutput-component.pkg"
pkgbuild --root "$STAGE" \
    --component-plist "$COMPONENT_PLIST" \
    --identifier "$PKG_ID" \
    --version "$VERSION" \
    --install-location "/Library/OFX/Plugins" \
    "$COMPONENT_PKG" >/dev/null

# Installer UI resources: license + readme
RES="$DIST/pkg_resources"
mkdir -p "$RES"
cp LICENSE "$RES/License.txt"
cat > "$RES/Readme.html" <<EOF
<!DOCTYPE html><html><body style="font-family:-apple-system, Helvetica, sans-serif; font-size:13px">
<h2>NDI Output for DaVinci Resolve v$VERSION</h2>
<p>This installs the <b>NDIOutput</b> OpenFX plugin into <code>/Library/OFX/Plugins</code>.
After installing, <b>restart DaVinci Resolve</b>, then find the plugin on the Color page
under OpenFX &rarr; LSVR &rarr; NDIOutput.</p>
<p>The plugin streams the rendered frame as an NDI&reg; source (SDR and HDR) on your local
network. Receive it with any NDI application &mdash; e.g. the free
<a href="https://ndi.video/tools/">NDI Tools</a>.</p>
<p>Requires macOS 13 or later (Apple Silicon or Intel). No other software is required:
the NDI runtime library is included.</p>
<p>Documentation &amp; source: <a href="https://github.com/lightsailvr/ResolveOFX_NDIOutput">github.com/lightsailvr/ResolveOFX_NDIOutput</a></p>
<hr>
<p style="font-size:11px">NDI&reg; is a registered trademark of Vizrt NDI AB &mdash;
<a href="https://ndi.video/">ndi.video</a>. Third-party license notices are installed at
<code>NDIOutput.ofx.bundle/Contents/Resources/libndi_licenses.txt</code>.</p>
</body></html>
EOF

DIST_XML="$DIST/distribution.xml"
cat > "$DIST_XML" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="1">
    <title>NDI Output for DaVinci Resolve $VERSION</title>
    <license file="License.txt"/>
    <readme file="Readme.html"/>
    <options customize="never" require-scripts="false" hostArchitectures="x86_64,arm64"/>
    <volume-check>
        <allowed-os-versions><os-version min="$DEPLOYMENT_TARGET"/></allowed-os-versions>
    </volume-check>
    <domains enable_localSystem="true"/>
    <choices-outline><line choice="default"><line choice="plugin"/></line></choices-outline>
    <choice id="default"/>
    <choice id="plugin" visible="false"><pkg-ref id="$PKG_ID"/></choice>
    <pkg-ref id="$PKG_ID" version="$VERSION" onConclusion="none">NDIOutput-component.pkg</pkg-ref>
</installer-gui-script>
EOF

if [ -n "${INSTALLER_CERT:-}" ]; then
    productbuild --distribution "$DIST_XML" \
        --resources "$RES" \
        --package-path "$DIST" \
        --sign "$INSTALLER_CERT" \
        "$PKG_OUT" >/dev/null
    echo "  built and signed: $PKG_OUT"
else
    PKG_OUT="$DIST/NDIOutput-$VERSION-macOS-UNSIGNED-DEV.pkg"
    productbuild --distribution "$DIST_XML" \
        --resources "$RES" \
        --package-path "$DIST" \
        "$PKG_OUT" >/dev/null
    echo "  built UNSIGNED dev pkg: $PKG_OUT"
fi
rm -f "$COMPONENT_PKG"

# Zip of the signed bare bundle for manual installs
ditto -c -k --keepParent "$BUNDLE" "$ZIP_OUT"
echo "  zipped signed bundle: $ZIP_OUT"

# -------------------------------------------------------------- notarize ----
if [ "$SKIP_NOTARIZE" -eq 0 ]; then
    echo "Notarizing pkg (this waits on Apple, typically 1-10 min)..."
    xcrun notarytool submit "$PKG_OUT" --keychain-profile "$NOTARY_PROFILE" --wait
    xcrun stapler staple "$PKG_OUT"
    echo "Notarizing zip (covers manual-install Gatekeeper checks)..."
    xcrun notarytool submit "$ZIP_OUT" --keychain-profile "$NOTARY_PROFILE" --wait
    # (zips can't be stapled; receivers validate online via the ticket)
    echo "Gatekeeper assessment:"
    spctl -a -vv -t install "$PKG_OUT"
else
    echo "Skipping notarization (--skip-notarize) — DO NOT distribute this pkg."
fi

# --------------------------------------------------------------- summary ----
(cd "$DIST" && shasum -a 256 "$(basename "$PKG_OUT")" "$(basename "$ZIP_OUT")" > SHA256SUMS.txt)
rm -rf "$STAGE" "$RES" "$COMPONENT_PLIST" "$DIST_XML"

echo ""
echo "Done. Release artifacts in $DIST/:"
ls -lh "$DIST"
