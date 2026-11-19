#!/bin/sh
# Regenerate patches/shogo-native.hpatch from edits made in build/gamesrc.
# Workflow: edit files under build/gamesrc, run this, then rebuild.
# The hashed patch quotes none of Monolith's source (tools/hpatch.py), so it
# goes in the repository; `tools/hpatch.py show` turns it into a normal diff.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC=${SHOGO_SOURCE_DIR:-$ROOT/third_party/shogsrc22/source}
BASE=$ROOT/build/gamesrc-baseline
python3 -I "$ROOT/tools/prepare_source.py" "$SRC" --no-patches "$BASE" >/dev/null
mkdir -p "$ROOT/patches"
python3 -I "$ROOT/tools/hpatch.py" make "$BASE" "$ROOT/build/gamesrc" "$ROOT/patches/shogo-native.hpatch"
