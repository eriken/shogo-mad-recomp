#!/bin/sh
# Regenerate patches/shogo-native.patch from edits made in build/gamesrc.
# Workflow: edit files under build/gamesrc, run this, then rebuild.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC=${SHOGO_SOURCE_DIR:-$ROOT/third_party/shogsrc22/source}
BASE=$ROOT/build/gamesrc-baseline
python3 -I "$ROOT/tools/prepare_source.py" "$SRC" --no-patches "$BASE" >/dev/null
mkdir -p "$ROOT/patches"
cd "$ROOT/build"
# diff exits 1 when files differ; that's the normal case.
diff -ruN -x .prepared gamesrc-baseline gamesrc | sed -e 's|^--- gamesrc-baseline/|--- a/|' -e 's|^+++ gamesrc/|+++ b/|' -e 's|^diff -ruN -x .prepared gamesrc-baseline/\([^ ]*\) gamesrc/.*|diff -ruN a/\1 b/\1|' > "$ROOT/patches/shogo-native.patch" || true
echo "$(grep -c '^+++ ' "$ROOT/patches/shogo-native.patch") files patched -> patches/shogo-native.patch"
