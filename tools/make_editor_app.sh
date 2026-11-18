#!/bin/sh
# Builds the editor as a double-clickable macOS app: build-export/AceEngine.app
# (Metal; SDL built in, so it does not depend on Homebrew libraries).
#   tools/make_editor_app.sh [output_dir]
# Copy the result to /Applications to keep it in Launchpad and the Dock.
# The app's File > Export still uses this source tree (tools/ and cmake), so it
# is meant for the machine it was built on.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/build-export}"
BUILD="$ROOT/build-export/editor-build"

cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DACE_RHI_VULKAN=OFF -DACE_FETCH_SDL=ON -DFETCHCONTENT_QUIET=ON >/dev/null
cmake --build "$BUILD" --target ace_editor

APP="$OUT/AceEngine.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BUILD/bin/ace_editor" "$APP/Contents/MacOS/AceEngine"
# SDL_GetBasePath() is Contents/Resources inside a bundle: shaders go there.
cp -R "$BUILD/bin/ace_data" "$APP/Contents/Resources/ace_data"
cp "$ROOT/editor/resources/AppIcon.icns" "$APP/Contents/Resources/AppIcon.icns"
VERSION="$(sed -n 's/^project(AceEngine VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>AceEngine</string>
  <key>CFBundleDisplayName</key><string>AceEngine</string>
  <key>CFBundleExecutable</key><string>AceEngine</string>
  <key>CFBundleIdentifier</key><string>com.aceengine.editor</string>
  <key>CFBundleIconFile</key><string>AppIcon</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>${VERSION:-0.0.0}</string>
  <key>CFBundleVersion</key><string>${VERSION:-0.0.0}</string>
  <key>LSMinimumSystemVersion</key><string>12.0</string>
  <key>NSHighResolutionCapable</key><true/>
</dict></plist>
PLIST
codesign --force --sign - "$APP" >/dev/null 2>&1 || true # ad-hoc signature: runs on this Mac without warnings
echo "built: $APP"
