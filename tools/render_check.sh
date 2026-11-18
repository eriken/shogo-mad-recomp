#!/bin/bash
# render_check.sh: screenshots every single-player level (and the main and
# options menus) with the Metal renderer and with the software reference,
# and diffs each pair.
#
#   tools/render_check.sh [--data DIR] [--out DIR] [--frames N] [--jobs N]
#                         [--max PERCENT] [--gpu-args "ARGS"] [LEVEL ...]
#
#   --data      the extracted game (default: gamedata in the repository)
#   --out       where screenshots, diffs and the report go
#               (default: build/render-check)
#   --frames    game frames before each screenshot (default 150)
#   --jobs      probes run at once (default 4)
#   --max       a scene fails when the average colour of more than PERCENT
#               of its 8x8 blocks differs by more than 8 of 255 (default 0.5).
#               Blocks rather than pixels: texture sampling that lands on the
#               neighbouring texel averages out, wrong surfaces don't.
#               tools/render_check.known allows more for named scenes.
#   --gpu-args  extra shogo-probe options for the Metal side, e.g.
#               "--smooth --msaa 4" (the diff is then for looking at, not
#               passing)
#   LEVEL       world names such as 01_Ambush, or menu / options; default:
#               every single-player world in SHOGO.REZ
#
# Writes OUT/<scene>_sw.png, _gpu.png and _diff.png (differences in red),
# OUT/summary.txt and OUT/index.html (side by side). Exits 1 when a scene
# fails or a probe doesn't produce its screenshot.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=$ROOT/build
DATA=$ROOT/gamedata
OUT=$BUILD/render-check
FRAMES=150
JOBS=4
MAX=0.5
GPU_ARGS=""
SCENES=()
while [ $# -gt 0 ]; do
    case $1 in
    --data) DATA=$2; shift 2 ;;
    --out) OUT=$2; shift 2 ;;
    --frames) FRAMES=$2; shift 2 ;;
    --jobs) JOBS=$2; shift 2 ;;
    --max) MAX=$2; shift 2 ;;
    --gpu-args) GPU_ARGS=$2; shift 2 ;;
    -h|--help) sed -n '2,28p' "$0"; exit 0 ;;
    *) SCENES+=("$1"); shift ;;
    esac
done
for f in shogo-probe shogo-imgdiff object.lto.dylib cshell.dylib; do
    [ -e "$BUILD/$f" ] || { echo "missing $BUILD/$f: build first" >&2; exit 2; }
done
[ -f "$DATA/SHOGO.REZ" ] || { echo "no SHOGO.REZ in $DATA (pass --data)" >&2; exit 2; }
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
DATA=$(cd "$DATA" && pwd)
if [ ${#SCENES[@]} -eq 0 ]; then
    SCENES=(menu options)
    while read -r w; do SCENES+=("$w"); done < <(
        python3 -I "$ROOT/tools/rezlist.py" "$DATA/SHOGO.REZ" |
            sed -n 's#.*WORLDS/\([^/]*\)\.DAT$#\1#p' | sort)
fi

# One screenshot: <scene> <sw|gpu>.
shoot() {
    local scene=$1 side=$2 args=()
    case $scene in
    menu) ;;
    options) args=(--key 40@40 --key 40@45 --key 13@50) ;;  # down, down, enter
    *) args=(+runworld "Worlds\\$scene") ;;
    esac
    if [ "$side" = gpu ]; then
        # shellcheck disable=SC2206
        args+=(--gpu $GPU_ARGS)
    else
        args+=(--gpu-reference)
    fi
    (cd "$DATA" && "$BUILD/shogo-probe" "${args[@]}" --frames "$FRAMES" --screenshot "$OUT/${scene}_$side.png" \
        "$BUILD/object.lto.dylib" "$BUILD/cshell.dylib" > "$OUT/${scene}_$side.log" 2>&1)
}
export -f shoot
export BUILD DATA OUT FRAMES GPU_ARGS

rm -f "$OUT"/*.png "$OUT"/*.log
printf '%s\n' "${SCENES[@]}" | sed 's/$/ sw/; p; s/ sw$/ gpu/' | xargs -P "$JOBS" -L 1 bash -c 'shoot "$0" "$1"'

failed=0
{
    printf '%-18s %s\n' scene "Metal against software"
    for s in "${SCENES[@]}"; do
        if [ ! -f "$OUT/${s}_sw.png" ] || [ ! -f "$OUT/${s}_gpu.png" ]; then
            printf '%-18s FAILED: no screenshot (see %s_*.log)\n' "$s" "$s"
            failed=1
            continue
        fi
        max=$MAX known=""
        if line=$(grep -i "^$s " "$ROOT/tools/render_check.known" 2>/dev/null); then
            max=$(echo "$line" | cut -d' ' -f2)
            known="  (known, up to $max%: $(echo "$line" | cut -d' ' -f3-))"
        fi
        result=$("$BUILD/shogo-imgdiff" "$OUT/${s}_sw.png" "$OUT/${s}_gpu.png" --diff "$OUT/${s}_diff.png" \
            --blocks 8 --max "$max")
        status=$?
        if [ $status -ne 0 ]; then
            failed=1
            result="$result  FAIL"
        else
            result="$result$known"
        fi
        printf '%-18s %s\n' "$s" "$result"
    done
} | tee "$OUT/summary.txt"
grep -q 'FAIL' "$OUT/summary.txt" && failed=1

{
    echo '<!doctype html><meta charset="utf-8"><title>Render check</title>'
    echo '<style>body{font:14px -apple-system,sans-serif;background:#16181d;color:#ddd;margin:16px}'
    echo 'h2{font-size:15px;margin:24px 0 6px}.fail{color:#f66}img{width:32%;image-rendering:pixelated}</style>'
    echo "<h1>Metal against software, frame $FRAMES</h1><p>Left: software. Middle: Metal${GPU_ARGS:+ ($GPU_ARGS)}. Right: differences in red.</p>"
    while read -r scene rest; do
        [ "$scene" = scene ] && continue
        cls=""; case $rest in *FAIL*) cls=' class="fail"' ;; esac
        echo "<h2$cls>$scene: $rest</h2>"
        echo "<img src=\"${scene}_sw.png\"><img src=\"${scene}_gpu.png\"> <img src=\"${scene}_diff.png\">"
    done < "$OUT/summary.txt"
} > "$OUT/index.html"
echo "Report: $OUT/index.html"
exit $failed
