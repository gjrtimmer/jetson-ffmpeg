#!/usr/bin/env bash
# Clone the supported FFmpeg release branches used by the smoke test.
#
# Each version is shallow-cloned into <dir>/ffmpeg<version>. Existing trees
# are left untouched (idempotent), so re-running only fetches what's missing.
#
# Usage:
#   test/clone-ffmpeg.sh [-d DIR] [-u URL] [VERSION ...]
#
#   -d DIR   Destination directory for the clones
#            (default: $FFMPEG_SRC_DIR, else $HOME/ffmpeg-smoke)
#   -u URL   FFmpeg git URL (default: https://git.ffmpeg.org/ffmpeg.git)
#   VERSION  One or more release versions (e.g. 6.0 8.0). With none given,
#            the full supported set is used.
#
# Safe to run from any working directory.
set -euo pipefail

DEST="${FFMPEG_SRC_DIR:-$HOME/ffmpeg-smoke}"
URL="https://git.ffmpeg.org/ffmpeg.git"
DEFAULT_VERSIONS="6.0 6.1 7.0 7.1 8.0"

while [ $# -gt 0 ]; do
    case "$1" in
        -d) DEST="$2"; shift ;;
        -u) URL="$2"; shift ;;
        -h|--help) sed -n '2,/^set /{/^set /d;s/^# \{0,1\}//;p}' "${BASH_SOURCE[0]}"; exit 0 ;;
        -*) echo "[E] unknown option: $1" >&2; exit 1 ;;
        *)  break ;;
    esac
    shift
done

VERSIONS="${*:-$DEFAULT_VERSIONS}"

mkdir -p "$DEST"
for v in $VERSIONS; do
    tree="$DEST/ffmpeg$v"
    if [ -d "$tree/.git" ]; then
        echo "[i] ffmpeg$v already present at $tree — skipping"
        continue
    fi
    echo "[i] cloning release/$v -> $tree"
    rm -rf "$tree" 2>/dev/null || sudo rm -rf "$tree" 2>/dev/null || true
    git clone --quiet --depth=1 -b "release/$v" "$URL" "$tree"
done
echo "[i] clones ready in $DEST"
