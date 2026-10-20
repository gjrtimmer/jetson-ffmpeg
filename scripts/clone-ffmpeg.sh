#!/usr/bin/env bash
# Clone the supported FFmpeg release branches (single FFmpeg fetch path for CI,
# smoke tests and patch regeneration).
#
# Each version is shallow-cloned into <dir>/ffmpeg<version>. Existing trees
# are left untouched (idempotent), so re-running only fetches what's missing.
#
# Resilience: up to 4 attempts per version with 5/15/45s backoff. Attempts 1-2
# use the primary URL (the second one over HTTP/1.1 after an HTTP2 framing
# error); attempts 3-4 use the GitHub mirror (same release/<ver> branch).
# The winning source and attempt are logged.
#
# Usage:
#   scripts/clone-ffmpeg.sh [-d DIR] [-o PATH] [-u URL] [-m URL] [VERSION ...]
#
#   -d DIR   Destination directory for the clones
#            (default: $FFMPEG_SRC_DIR, else $HOME/ffmpeg-smoke)
#   -o PATH  Clone exactly ONE version into PATH (instead of DIR/ffmpeg<ver>)
#   -u URL   Primary FFmpeg git URL (default: https://git.ffmpeg.org/ffmpeg.git)
#   -m URL   Fallback mirror URL (default: https://github.com/FFmpeg/FFmpeg.git)
#   VERSION  One or more release versions (e.g. 6.0 8.0). With none given,
#            the full supported set is used.
#
# Env: FFMPEG_CLONE_BACKOFF="5 15 45" overrides the sleeps between attempts.
#
# Safe to run from any working directory.
set -euo pipefail

DEST="${FFMPEG_SRC_DIR:-$HOME/ffmpeg-smoke}"
OUT=""
URL="https://git.ffmpeg.org/ffmpeg.git"
MIRROR="https://github.com/FFmpeg/FFmpeg.git"
BACKOFF="${FFMPEG_CLONE_BACKOFF:-5 15 45}"
DEFAULT_VERSIONS="6.0 6.1 7.0 7.1 8.0 8.1 9.0"

while [ $# -gt 0 ]; do
    case "$1" in
        -d) DEST="$2"; shift ;;
        -o) OUT="$2"; shift ;;
        -u) URL="$2"; shift ;;
        -m) MIRROR="$2"; shift ;;
        -h|--help) sed -n '2,/^set /{/^set /d;s/^# \{0,1\}//;p}' "${BASH_SOURCE[0]}"; exit 0 ;;
        -*) echo "[E] unknown option: $1" >&2; exit 1 ;;
        *)  break ;;
    esac
    shift
done

VERSIONS="${*:-$DEFAULT_VERSIONS}"
if [ -n "$OUT" ] && [ "$(wc -w <<<"$VERSIONS")" -ne 1 ]; then
    echo "[E] -o requires exactly one VERSION" >&2
    exit 1
fi

# clone_one VERSION TREE: retry/fallback fetch of release/VERSION into TREE.
clone_one() {
    local v="$1" tree="$2" tmp log attempt=0 src http1=0 delay
    local -a sleeps
    read -r -a sleeps <<<"$BACKOFF"
    tmp="$tree.tmp.$$"
    log="$(mktemp)"
    # shellcheck disable=SC2064
    trap "rm -rf '$tmp' '$log'" RETURN
    while [ "$attempt" -lt 4 ]; do
        attempt=$((attempt + 1))
        if [ "$attempt" -le 2 ]; then src="$URL"; else src="$MIRROR"; fi
        if [ "$attempt" -gt 1 ]; then
            delay="${sleeps[$((attempt - 2))]:-45}"
            echo "[w] attempt $((attempt - 1)) failed; retry in ${delay}s"
            sleep "$delay"
        fi
        rm -rf "$tmp"
        local -a opts=()
        # Switch to HTTP/1.1 once an HTTP2 framing error has been seen.
        if [ "$http1" -eq 1 ]; then opts=(-c http.version=HTTP/1.1); fi
        echo "[i] ffmpeg$v attempt $attempt/4: $src${opts[*]:+ (HTTP/1.1)}"
        if git "${opts[@]}" clone --quiet --depth=1 -b "release/$v" "$src" "$tmp" 2>"$log"; then
            mv "$tmp" "$tree"
            echo "[i] ffmpeg$v cloned from $src on attempt $attempt"
            return 0
        fi
        cat "$log" >&2
        if grep -qiE 'HTTP2 framing|http/2 stream' "$log"; then http1=1; fi
    done
    echo "[E] ffmpeg$v: all 4 attempts failed" >&2
    return 1
}

mkdir -p "$DEST"
for v in $VERSIONS; do
    tree="${OUT:-$DEST/ffmpeg$v}"
    if [ -d "$tree/.git" ]; then
        echo "[i] ffmpeg$v already present at $tree — skipping"
        continue
    fi
    echo "[i] cloning release/$v -> $tree"
    mkdir -p "$(dirname "$tree")"
    rm -rf "$tree" 2>/dev/null || sudo rm -rf "$tree" 2>/dev/null || true
    clone_one "$v" "$tree"
done
echo "[i] clones ready in ${OUT:-$DEST}"
