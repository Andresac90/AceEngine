#!/bin/sh
# Exports a project as an iOS .app (Metal).
#   tools/export_ios.sh <project_dir> [simulator|device] [output_dir]
#
# simulator (default): runs in the iOS Simulator, no signing needed:
#     xcrun simctl boot "iPhone 17"; xcrun simctl install booted <App>.app
#     xcrun simctl launch booted <bundle id printed below>
# device: produces an UNSIGNED .app. Sign it with your Apple developer identity
#     and provisioning profile (codesign / Xcode) before it can be installed.
set -eu
[ $# -ge 1 ] || { echo "usage: $0 <project_dir> [simulator|device] [output_dir]" >&2; exit 2; }
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PROJECT="$(cd "$1" && pwd)"
TARGET="${2:-simulator}"
case "$TARGET" in
  simulator) SDK=iphonesimulator; PLATFORM=iPhoneSimulator ;;
  device) SDK=iphoneos; PLATFORM=iPhoneOS ;;
  *) echo "error: target must be 'simulator' or 'device'" >&2; exit 2 ;;
esac
OUT="${3:-$ROOT/build-export/ios-$TARGET}"
[ -f "$PROJECT/project.json" ] || { echo "error: $PROJECT has no project.json" >&2; exit 1; }
NAME="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["name"])' "$PROJECT/project.json")"
BUILD="$ROOT/build-export/ios-$TARGET-build"
MIN_IOS=15.0

cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT="$SDK" -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="$MIN_IOS" -DFETCHCONTENT_QUIET=ON >/dev/null
cmake --build "$BUILD" --target ace_player

# An iOS bundle is flat: SDL_GetBasePath() is the .app folder itself.
APP="$OUT/$NAME.app"
EXE="$(printf '%s' "$NAME" | tr -cd 'A-Za-z0-9')"
BUNDLE_ID="com.aceengine.$(printf '%s' "$EXE" | tr 'A-Z' 'a-z')"
rm -rf "$APP"
mkdir -p "$APP"
# CMake wraps iOS executables in their own bare .app; take just the binary.
cp "$BUILD/bin/ace_player.app/ace_player" "$APP/$EXE"
cp -R "$BUILD/bin/ace_data" "$APP/ace_data"
cp -R "$PROJECT" "$APP/project"
cat > "$APP/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>$NAME</string>
  <key>CFBundleDisplayName</key><string>$NAME</string>
  <key>CFBundleExecutable</key><string>$EXE</string>
  <key>CFBundleIdentifier</key><string>$BUNDLE_ID</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>1.0</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>CFBundleSupportedPlatforms</key><array><string>$PLATFORM</string></array>
  <key>MinimumOSVersion</key><string>$MIN_IOS</string>
  <key>UIDeviceFamily</key><array><integer>1</integer><integer>2</integer></array>
  <key>UILaunchScreen</key><dict/>
  <key>UIRequiresFullScreen</key><true/>
  <key>UIApplicationSupportsIndirectInputEvents</key><true/>
  <key>UISupportedInterfaceOrientations</key><array>
    <string>UIInterfaceOrientationLandscapeLeft</string><string>UIInterfaceOrientationLandscapeRight</string>
  </array>
</dict></plist>
PLIST
[ "$TARGET" = simulator ] && codesign --force --sign - "$APP" >/dev/null 2>&1 || true
echo "exported: $APP"
echo "bundle id: $BUNDLE_ID"
