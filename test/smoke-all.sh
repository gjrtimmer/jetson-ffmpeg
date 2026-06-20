#!/usr/bin/env bash
# Full smoke test across every supported FFmpeg version.
#
# For each version it: ensures a clone (scripts/clone-ffmpeg.sh) -> resets it to
# pristine -> patches it (scripts/ffpatch.sh) -> configures with nvmpi -> builds
# -> runs every hardware suite via test/hw-all.sh. A pass/fail matrix is
# printed at the end; the script exits non-zero if any version fails.
#
# This is the heaviest test in the repo: it builds libnvmpi and a full FFmpeg
# for each version, and the hw stages REQUIRE a real Jetson (the nvmpi
# codecs have no software fallback).
#
# Prerequisites:
#   - A real Jetson with the Multimedia API (for the hw stages).
#   - Build deps for FFmpeg incl. libx264 (the dev container installs these;
#     in CI they are apt-installed). libx264 is needed because the hw suites
#     generate their H.264 input with libx264. libx265 is optional — enabled
#     automatically when its dev headers are present — and lets the
#     format-pixfmt suite generate a 10-bit HEVC sample for the P010 case;
#     without it that one case is skipped.
#
# Usage:
#   test/smoke-all.sh [options]
#
#   -d DIR        Scratch dir for clones/builds
#                 (default: $FFMPEG_SRC_DIR, else $HOME/ffmpeg-smoke)
#   -j N          Parallel build jobs (default: nproc)
#   -v "LIST"     Space-separated versions (default: full supported set)
#   --no-nvmpi    Skip building/installing libnvmpi (assume already installed)
#   -h, --help    Show this help
#
# Safe to run from any working directory.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # <repo>/test
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

DEST="${FFMPEG_SRC_DIR:-$HOME/ffmpeg-smoke}"
JOBS="$(nproc)"
VERSIONS="6.0 6.1 7.0 7.1 8.0"
BUILD_NVMPI=1

while [ $# -gt 0 ]; do
    case "$1" in
        -d) DEST="$2"; shift ;;
        -j) JOBS="$2"; shift ;;
        -v) VERSIONS="$2"; shift ;;
        --no-nvmpi) BUILD_NVMPI=0 ;;
        -h|--help) sed -n '2,/^set /{/^set /d;s/^# \{0,1\}//;p}' "${BASH_SOURCE[0]}"; exit 0 ;;
        *) echo "[E] unknown option: $1" >&2; exit 1 ;;
    esac
    shift
done
export FFMPEG_SRC_DIR="$DEST"

# libnvmpi must be installed so FFmpeg's pkg-config finds it and the built
# binaries can link against it at runtime.
if [ "$BUILD_NVMPI" -eq 1 ]; then
    echo "[i] building + installing libnvmpi"
    "${REPO_ROOT}/scripts/build.sh" --install -j "$JOBS"
fi
export LD_LIBRARY_PATH="/usr/local/lib:/usr/lib/aarch64-linux-gnu/tegra:${LD_LIBRARY_PATH:-}"

# Ensure all required FFmpeg trees are cloned.
# shellcheck disable=SC2086
"${REPO_ROOT}/scripts/clone-ffmpeg.sh" -d "$DEST" $VERSIONS

SUMMARY=""
fail=0
for v in $VERSIONS; do
    d="$DEST/ffmpeg$v"
    patch_r=skip conf_r=skip build_r=skip hw_r=skip
    echo "######## ffmpeg $v ########"

    # Reset to pristine FFmpeg (drop our patch + prior build artifacts).
    git -C "$d" reset --hard >/dev/null 2>&1
    git -C "$d" clean -fdx   >/dev/null 2>&1

    if "${REPO_ROOT}/scripts/ffpatch.sh" "$d"; then patch_r=OK; else patch_r=FAIL; fi

    if [ "$patch_r" = OK ]; then
        # libx265 is optional: only enable it when the dev headers are present
        # (the format-pixfmt suite uses it to generate a 10-bit HEVC sample for
        # the P010 case, and skips that case when it is absent).
        x265_flag=""
        if pkg-config --exists x265 2>/dev/null; then x265_flag="--enable-libx265"; fi
        if ( cd "$d" && ./configure --enable-nvmpi --enable-gpl --enable-libx264 $x265_flag --disable-doc ); then
            conf_r=OK
        else conf_r=FAIL; fi
    fi

    if [ "$conf_r" = OK ]; then
        if ( cd "$d" && make -j"$JOBS" ); then build_r=OK; else build_r=FAIL; fi
    fi

    if [ "$build_r" = OK ]; then
        if PATH="$d:$PATH" JETSON_VARIANT="smoke-$v" bash "${SCRIPT_DIR}/hw-all.sh"; then
            hw_r=PASS
        else hw_r=FAIL; fi
    fi

    line="ffmpeg $v : patch=$patch_r configure=$conf_r build=$build_r hw-all=$hw_r"
    SUMMARY="${SUMMARY}${line}"$'\n'
    case "$line" in *FAIL*) fail=1 ;; esac
done

echo ""
echo "================ SMOKE MATRIX ================"
printf '%s' "$SUMMARY"
if [ "$fail" -eq 0 ]; then echo "RESULT: ALL GREEN"; else echo "RESULT: FAILURES PRESENT"; fi
exit "$fail"
