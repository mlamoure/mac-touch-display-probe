#!/bin/bash
# The touch test app (TouchProbe.swift; gillesgoetsch/OmacVM#843), for a Mac with a touch display
# or a Sidecar iPad on iPadOS 27: TouchProbe.app in OUT (default: ./build).
# Steps A to F; "Skip to the fix" (or open --args --fix) goes to step F.
# Step F runs OmacVM's touch rules (omacvm-touch.h, the header that
# omacvm-cocoa-touch-logic.patch adds to QEMU), compiled in. Needs Xcode or
# its Command Line Tools; signed ad hoc, nothing installed.
#   build.sh [OUT]       then: open OUT/TouchProbe.app
#   python3 analyze.py ~/Library/Logs/touch-probe-<date>-<time>.tsv
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd -P)
out=${1:-$here/build}
work=$(mktemp -d "${TMPDIR:-/tmp}/touch-probe.XXXXXX")
trap 'rm -rf "$work"' EXIT
mkdir -p "$out"
xcrun swiftc -O -swift-version 5 -import-objc-header "$here/omacvm-touch.h" \
  -framework AppKit "$here/TouchProbe.swift" -o "$work/TouchProbe"
v=TouchProbe
app="$out/$v.app"
rm -rf "$app"
mkdir -p "$app/Contents/MacOS"
cp "$work/TouchProbe" "$app/Contents/MacOS/$v"
cat > "$app/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>$v</string>
<key>CFBundleIdentifier</key><string>local.touchprobe.$v</string>
<key>CFBundleName</key><string>$v</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>1.0</string>
<key>LSApplicationCategoryType</key><string>public.app-category.developer-tools</string>
<key>NSHighResolutionCapable</key><true/>
<key>NSPrincipalClass</key><string>NSApplication</string>
</dict></plist>
PLIST
codesign -s - --force "$app" >/dev/null 2>&1
echo "built: $app"
