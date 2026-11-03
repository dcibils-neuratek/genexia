#!/bin/bash
# Builds "Genexia Recompiler.app" next to this script's repo root.
set -eo pipefail
cd "$(dirname "$0")"
ROOT="$(cd ../.. && pwd)"
swift build -c release 2>&1 | tail -3
APP="$ROOT/Genexia Recompiler.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS"
cp .build/release/GenexiaRecompiler "$APP/Contents/MacOS/"
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleName</key><string>Genexia Recompiler</string>
<key>CFBundleIdentifier</key><string>com.genexia.recompiler</string>
<key>CFBundleExecutable</key><string>GenexiaRecompiler</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>1.0</string>
<key>LSMinimumSystemVersion</key><string>14.0</string>
<key>NSHighResolutionCapable</key><true/>
<key>GenexiaRepo</key><string>$ROOT</string>
</dict></plist>
PLIST
# A real identity keeps macOS's folder permissions across rebuilds; ad-hoc looks like a new app each time.
ID="$(security find-identity -v -p codesigning | awk -F'"' 'NR==1 && NF>1 {print $2}')"
if [ -z "$ID" ] || ! codesign -s "$ID" "$APP" 2>/dev/null; then codesign -s - "$APP" 2>/dev/null || true; fi
echo "$APP"
