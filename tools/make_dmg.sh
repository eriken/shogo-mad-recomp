#!/bin/sh
# Pack build/Shogo.app into a disk image for a release: the app (launcher,
# engine, compiled game modules and SDL3) and a link to Applications. No game
# source and no game data: players import their own copy of Shogo from GOG.
# Usage: tools/make_dmg.sh [BUILD_DIR] [OUT.dmg]
# With SHOGO_SIGN_IDENTITY set (the app's Developer ID identity), the disk
# image is signed too; then tools/notarize.sh sends it to Apple.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${1:-$ROOT/build}
APP=$BUILD/Shogo.app
VERSION=$(/usr/libexec/PlistBuddy -c 'Print CFBundleShortVersionString' "$APP/Contents/Info.plist")
OUT=${2:-$BUILD/Shogo-$VERSION.dmg}
[ -d "$APP" ] || { echo "no $APP: build first (cmake --build build)" >&2; exit 1; }

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"
rm -f "$OUT"
hdiutil create -volname "Shogo" -srcfolder "$STAGE" -fs HFS+ -format UDZO -ov "$OUT" >/dev/null
if [ -n "$SHOGO_SIGN_IDENTITY" ]; then
    codesign --force --timestamp -s "$SHOGO_SIGN_IDENTITY" "$OUT"
fi
echo "$OUT"
