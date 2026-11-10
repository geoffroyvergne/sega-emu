#!/bin/sh
# Smoke-tests every ROM in a folder: runs each one headless (no window) and
# flags those whose screen is still blank after 30 s of emulated time.
#   usage: tools/check_games.sh <rom-folder> [frames]   (default 1800 = 30 s)
set -u
dir=${1:?usage: $0 <rom-folder> [frames]}
frames=${2:-1800}
here=$(cd "$(dirname "$0")/.." && pwd)
bin="$here/build-release/frontend/megadrive"
[ -x "$bin" ] || { echo "Build Release first: $bin not found"; exit 2; }
out=$(mktemp -d)
bad=0
for rom in "$dir"/*; do
    [ -f "$rom" ] || continue
    shot="$out/shot.ppm"
    rm -f "$shot"
    if ! "$bin" --frames "$frames" --screenshot "$shot" "$rom" >/dev/null 2>&1 || [ ! -f "$shot" ]; then
        echo "CRASH  $(basename "$rom")"; bad=$((bad + 1)); continue
    fi
    # Count distinct colours on a sample of pixels: a blank screen has 1-2.
    colours=$(python3 -I -c '
import sys
d = open(sys.argv[1], "rb").read().split(b"\n", 3)[3]
print(len({d[i:i+3] for i in range(0, len(d), 21)}))' "$shot")
    if [ "$colours" -lt 3 ]; then
        echo "BLANK  $(basename "$rom")"; bad=$((bad + 1))
    else
        echo "ok     $(basename "$rom")  ($colours colours)"
    fi
done
rm -rf "$out"
echo "$bad problem(s)"
[ "$bad" -eq 0 ]
