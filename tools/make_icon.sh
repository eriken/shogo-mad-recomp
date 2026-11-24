#!/bin/sh
# Rebuilds assets/AppIcon.icns from assets/AppIcon.svg (the app's own icon,
# built into Shogo.app by cmake/make_app.cmake). The .icns is committed, so a
# build needs no image tools; run this only after changing the SVG.
# Usage: tools/make_icon.sh
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SVG=$ROOT/assets/AppIcon.svg
SET=$(mktemp -d)/AppIcon.iconset
trap 'rm -rf "$(dirname "$SET")"' EXIT
mkdir -p "$SET"
for s in 16 32 128 256 512; do
    swift "$ROOT/tools/svg_to_png.swift" "$SVG" "$SET/icon_${s}x${s}.png" $s
    swift "$ROOT/tools/svg_to_png.swift" "$SVG" "$SET/icon_${s}x${s}@2x.png" $((s * 2))
done
iconutil -c icns -o "$ROOT/assets/AppIcon.icns" "$SET"
echo "$ROOT/assets/AppIcon.icns"
