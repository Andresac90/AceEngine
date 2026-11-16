#!/bin/sh
# Exports a project as a self-contained macOS .app (Metal, SDL linked statically).
#   tools/export_macos.sh <project_dir> [output_dir]
set -eu
[ $# -ge 1 ] || { echo "usage: $0 <project_dir> [output_dir]" >&2; exit 2; }
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PROJECT="$(cd "$1" && pwd)"
OUT="${2:-$ROOT/build-export/macos}"
[ -f "$PROJECT/project.json" ] || { echo "error: $PROJECT has no project.json" >&2; exit 1; }
NAME="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["name"])' "$PROJECT/project.json")"
BUILD="$ROOT/build-export/macos-build"

cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DACE_BUILD_EDITOR=OFF -DACE_RHI_VULKAN=OFF -DACE_FETCH_SDL=ON -DFETCHCONTENT_QUIET=ON >/dev/null
cmake --build "$BUILD" --target ace_player

APP="$OUT/$NAME.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BUILD/bin/ace_player" "$APP/Contents/MacOS/$NAME"
# SDL_GetBasePath() points at Contents/Resources inside a bundle.
cp -R "$BUILD/bin/ace_data" "$APP/Contents/Resources/ace_data"
cp -R "$PROJECT" "$APP/Contents/Resources/project"
BUNDLE_ID="com.aceengine.$(printf '%s' "$NAME" | tr -cd 'A-Za-z0-9' | tr 'A-Z' 'a-z')"
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>$NAME</string>
  <key>CFBundleExecutable</key><string>$NAME</string>
  <key>CFBundleIdentifier</key><string>$BUNDLE_ID</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>1.0</string>
  <key>NSHighResolutionCapable</key><true/>
</dict></plist>
PLIST
echo "exported: $APP"
