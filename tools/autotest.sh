#!/bin/sh
# Runs the editor's scripted interaction test on a throwaway copy of the demo
# project, once per backend given (default: metal vulkan). Needs a display.
#   tools/autotest.sh [backend...]
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
[ $# -gt 0 ] || set -- metal vulkan
for backend in "$@"; do
  rm -rf "$TMP/demo" && cp -R "$ROOT/projects/pixel_demo" "$TMP/demo"
  # The test script drives entities of the small "main" scene.
  sed -i.bak 's|"startScene": *"[^"]*"|"startScene": "scenes/main.scene.json"|' "$TMP/demo/project.json"
  if (cd "$TMP" && "$ROOT/build/bin/ace_editor" "$TMP/demo" --backend "$backend" --autotest >"$TMP/log" 2>&1); then
    echo "editor autotest ($backend): passed"
  else
    echo "editor autotest ($backend): FAILED" >&2
    grep -E "error|FAILED" "$TMP/log" >&2 || cat "$TMP/log" >&2
    exit 1
  fi
done
