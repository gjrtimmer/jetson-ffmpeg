#!/usr/bin/env bash
# Patch and build a full FFmpeg with nvmpi support.
#
# libnvmpi is NOT built here: it lives in timmertech/jetson-nvmpi (GitHub mirror
# gjrtimmer/jetson-nvmpi). FFmpeg loads it at runtime with dlopen
# (libnvmpi.so.1, then libnvmpi.so), so building FFmpeg needs no libnvmpi
# headers or libraries; running it needs a libnvmpi installed from one of:
#   - the libnvmpi1 deb of a jetson-nvmpi release (sudo apt install ./libnvmpi1_*.deb),
#   - a release tarball unpacked into a prefix (scripts/fetch-nvmpi.sh), passed
#     here with --nvmpi-prefix so the build and the post-build checks find it.
#
# It clones (or reuses) the requested FFmpeg version, patches it with
# scripts/ffpatch.sh, configures it with --enable-nvmpi, and builds it.
#
# Usage:
#   scripts/build.sh --ffmpeg VER|PATH [options]
#
# Options:
#   --ffmpeg VER|PATH  FFmpeg to build. VER (e.g. 7.1) is cloned; a PATH to an
#                      existing tree is patched in place. Required.
#   --nvmpi-prefix D   Prefix holding an installed libnvmpi (lib/, optional
#                      lib/pkgconfig), e.g. from scripts/fetch-nvmpi.sh. Put on
#                      LD_LIBRARY_PATH / PKG_CONFIG_PATH for the build. Default:
#                      the system libnvmpi (libnvmpi1 deb).
#   --install          Install FFmpeg (uses sudo if not root) + ldconfig.
#   --ffmpeg-dir DIR   Where to clone FFmpeg (default: $FFMPEG_SRC_DIR, else
#                      $HOME/ffmpeg-build).
#   --ffmpeg-prefix D  FFmpeg --prefix used when --install is given.
#   --ffmpeg-args "A"  Extra ./configure args appended to the FFmpeg defaults.
#   --no-libx264       Drop the default FFmpeg "--enable-gpl --enable-libx264".
#   --no-libx265       Never add "--enable-libx265" (otherwise auto-enabled
#                      when its dev headers are present). libx265 is optional
#                      (software HEVC encoder); it is NOT needed for nvmpi or
#                      for P010 hardware decode — only to generate the 10-bit
#                      HEVC test sample in test/format/hw-format-pixfmt.sh.
#   -j N               Parallel build jobs (default: nproc).
#   -h, --help         Show this help.
#
# Quick builds:
#   scripts/build.sh --ffmpeg 7.1                      # FFmpeg 7.1 + nvmpi, system libnvmpi
#   scripts/fetch-nvmpi.sh --version 4.2.1 --dest ~/nvmpi
#   scripts/build.sh --ffmpeg 7.1 --nvmpi-prefix ~/nvmpi
#
# Safe to run from any working directory.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# Defaults
JOBS="$(nproc)"
DO_INSTALL=0
NVMPI_PREFIX=""
# JP6 moved tegra libs from tegra/ to nvidia/; auto-detect
if [ -d "/usr/lib/aarch64-linux-gnu/nvidia" ]; then
    _DEFAULT_LIB_DIR="/usr/lib/aarch64-linux-gnu/nvidia"
else
    _DEFAULT_LIB_DIR="/usr/lib/aarch64-linux-gnu/tegra"
fi
JETSON_LIB_DIR="${JETSON_MULTIMEDIA_LIB_DIR:-${_DEFAULT_LIB_DIR}}"
# FFmpeg
FFMPEG_TARGET=""
FFMPEG_DIR="${FFMPEG_SRC_DIR:-$HOME/ffmpeg-build}"
FFMPEG_PREFIX=""
FFMPEG_ARGS=""
WITH_X264=1
WITH_X265="auto"   # auto|1|0; auto = enable --enable-libx265 if pkg-config finds x265

usage() { sed -n '2,/^set /{/^set /d;s/^# \{0,1\}//;p}' "${BASH_SOURCE[0]}"; }

while [ $# -gt 0 ]; do
    case "$1" in
        --install)      DO_INSTALL=1 ;;
        --nvmpi-prefix) NVMPI_PREFIX="$2"; shift ;;
        --ffmpeg)       FFMPEG_TARGET="$2"; shift ;;
        --ffmpeg-dir)   FFMPEG_DIR="$2"; shift ;;
        --ffmpeg-prefix) FFMPEG_PREFIX="$2"; shift ;;
        --ffmpeg-args)  FFMPEG_ARGS="$2"; shift ;;
        --no-libx264)   WITH_X264=0 ;;
        --no-libx265)   WITH_X265=0 ;;
        -j)             JOBS="$2"; shift ;;
        -j*)            JOBS="${1#-j}" ;;
        -h|--help)      usage; exit 0 ;;
        *) echo "[E] unknown option: $1" >&2; usage; exit 1 ;;
    esac
    shift
done

if [ -z "${FFMPEG_TARGET}" ]; then
    echo "[E] --ffmpeg VER|PATH is required (libnvmpi is built in jetson-nvmpi)" >&2
    usage
    exit 1
fi

SUDO=""
[ "$(id -u)" -ne 0 ] && SUDO="sudo"

if [ -n "${NVMPI_PREFIX}" ]; then
    if ! ls "${NVMPI_PREFIX}"/lib/libnvmpi.so* >/dev/null 2>&1; then
        echo "[E] no libnvmpi in ${NVMPI_PREFIX}/lib (see scripts/fetch-nvmpi.sh)" >&2
        exit 1
    fi
    echo "[i] Using libnvmpi from ${NVMPI_PREFIX}"
elif ! ldconfig -p 2>/dev/null | grep -q 'libnvmpi\.so'; then
    echo "[w] no system libnvmpi found (install libnvmpi1 from a jetson-nvmpi release or pass --nvmpi-prefix);" >&2
    echo "[w] FFmpeg still builds, but the nvmpi codecs fail to load at runtime." >&2
fi

# ---------------------------------------------------------------------------
# FFmpeg build
# ---------------------------------------------------------------------------
# Resolve the FFmpeg tree: an existing dir is patched in place; otherwise the
# argument is treated as a release version and cloned via scripts/clone-ffmpeg.sh.
if [ -d "${FFMPEG_TARGET}" ]; then
    FF_DIR="$(cd "${FFMPEG_TARGET}" && pwd)"
    echo "[i] Using existing FFmpeg tree: ${FF_DIR}"
else
    "${REPO_ROOT}/scripts/clone-ffmpeg.sh" -d "${FFMPEG_DIR}" "${FFMPEG_TARGET}"
    FF_DIR="${FFMPEG_DIR}/ffmpeg${FFMPEG_TARGET}"
fi

# Let the build and the post-build checks see the chosen libnvmpi.
if [ -n "${NVMPI_PREFIX}" ]; then
    export PKG_CONFIG_PATH="${NVMPI_PREFIX}/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
    export LD_LIBRARY_PATH="${NVMPI_PREFIX}/lib:${JETSON_LIB_DIR}:${LD_LIBRARY_PATH:-}"
else
    export LD_LIBRARY_PATH="${JETSON_LIB_DIR}:${LD_LIBRARY_PATH:-}"
fi

"${REPO_ROOT}/scripts/ffpatch.sh" "${FF_DIR}"

# NVIDIA's libv4l2.h uses #if HAVE_VISIBILITY without defining the macro;
# suppress the resulting -Wundef warning (GCC visibility is available on all
# Jetson toolchains).
FF_CONF=(--enable-nvmpi --disable-doc --extra-cflags="-DHAVE_VISIBILITY=1")
[ "${WITH_X264}" -eq 1 ] && FF_CONF+=(--enable-gpl --enable-libx264)
# libx265 is optional (software HEVC encoder; used only to generate the 10-bit
# HEVC test sample). auto = enable when its dev headers are present, so a
# build.sh-built FFmpeg can run the full test suite; never required otherwise.
if [ "${WITH_X265}" = auto ]; then
    if pkg-config --exists x265 2>/dev/null; then WITH_X265=1; else WITH_X265=0; fi
fi
[ "${WITH_X265}" -eq 1 ] && FF_CONF+=(--enable-gpl --enable-libx265)
[ -n "${FFMPEG_PREFIX}" ] && FF_CONF+=("--prefix=${FFMPEG_PREFIX}")
# shellcheck disable=SC2206
[ -n "${FFMPEG_ARGS}" ] && FF_CONF+=(${FFMPEG_ARGS})

echo "[i] Configuring FFmpeg: ${FF_CONF[*]}"
( cd "${FF_DIR}" && ./configure "${FF_CONF[@]}" )

echo "[i] Building FFmpeg with ${JOBS} jobs"
( cd "${FF_DIR}" && make -j"${JOBS}" )

if [ "${DO_INSTALL}" -eq 1 ]; then
    echo "[i] Installing FFmpeg"
    ( cd "${FF_DIR}" && ${SUDO} make install )
    ${SUDO} ldconfig || true
fi

echo "[i] Done. FFmpeg built at ${FF_DIR}/ffmpeg"
