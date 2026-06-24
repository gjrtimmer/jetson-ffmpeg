#!/bin/bash
# Build (and optionally install) the libnvmpi shared library, and optionally a
# full FFmpeg with nvmpi support.
#
# By default it builds libnvmpi against the real Jetson Multimedia API
# libraries; if those are not present (e.g. building off-Jetson or in CI) it
# automatically falls back to the stubs in stubs/ so the library still compiles
# (but is NOT runnable).
#
# With --ffmpeg it goes further: after building + installing libnvmpi, it
# clones (or reuses) the requested FFmpeg version, patches it with
# scripts/ffpatch.sh, configures it with --enable-nvmpi, and builds it.
#
# Usage:
#   scripts/build.sh [options]
#
# libnvmpi options:
#   --stubs            Force linking against stubs/ (-DWITH_STUBS=ON).
#   --no-stubs         Force linking against real Jetson libraries.
#   --install          Install (uses sudo if not root) + ldconfig. With --ffmpeg
#                      this also installs the built ffmpeg.
#   --clean            Remove the build directory before configuring.
#   --package          Build a .deb package (CPack) after building.
#   --build-dir DIR    libnvmpi build directory (default: <repo>/build).
#   --prefix DIR       libnvmpi CMAKE_INSTALL_PREFIX (default: /usr/local).
#   --build-type TYPE  CMAKE_BUILD_TYPE (default: Release).
#   -j N               Parallel build jobs (default: nproc).
#   -h, --help         Show this help.
#
# FFmpeg options (only meaningful with --ffmpeg):
#   --ffmpeg VER|PATH  Also build FFmpeg with nvmpi. VER (e.g. 7.1) is cloned;
#                      a PATH to an existing tree is patched in place. Implies
#                      installing libnvmpi (FFmpeg must find it via pkg-config).
#   --ffmpeg-dir DIR   Where to clone FFmpeg (default: $FFMPEG_SRC_DIR, else
#                      $HOME/ffmpeg-build).
#   --ffmpeg-prefix D  FFmpeg --prefix used when --install is given.
#   --ffmpeg-args "A"  Extra ./configure args appended to the FFmpeg defaults.
#   --no-libx264       Drop the default FFmpeg "--enable-gpl --enable-libx264".
#   --no-libx265       Never add "--enable-libx265" (otherwise auto-enabled
#                      when its dev headers are present). libx265 is optional
#                      (software HEVC encoder); it is NOT needed for nvmpi or
#                      for P010 hardware decode — only to generate the 10-bit
#                      HEVC test sample in test/hw-format-pixfmt.sh.
#
# Quick builds:
#   scripts/build.sh                 # libnvmpi only
#   scripts/build.sh --install       # libnvmpi, installed system-wide
#   scripts/build.sh --stubs         # libnvmpi against stubs (off-Jetson / CI)
#   scripts/build.sh --ffmpeg 7.1    # libnvmpi + FFmpeg 7.1 with nvmpi
#
# Safe to run from any working directory.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# Defaults
BUILD_DIR="${REPO_ROOT}/build"
BUILD_TYPE="Release"
JOBS="$(nproc)"
DO_INSTALL=0
DO_CLEAN=0
DO_PACKAGE=0
PREFIX="/usr/local"   # CMake's default install prefix; override with --prefix
WITH_STUBS=""   # "", "ON" or "OFF"; "" means auto-detect
JETSON_API_DIR="${JETSON_MULTIMEDIA_API_DIR:-/usr/src/jetson_multimedia_api}"
# JP6 moved tegra libs from tegra/ to nvidia/; auto-detect
if [ -d "/usr/lib/aarch64-linux-gnu/nvidia" ]; then
    _DEFAULT_LIB_DIR="/usr/lib/aarch64-linux-gnu/nvidia"
elif [ -d "/usr/lib/aarch64-linux-gnu/tegra" ]; then
    _DEFAULT_LIB_DIR="/usr/lib/aarch64-linux-gnu/tegra"
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
        --stubs)        WITH_STUBS="ON" ;;
        --no-stubs)     WITH_STUBS="OFF" ;;
        --install)      DO_INSTALL=1 ;;
        --package)      DO_PACKAGE=1 ;;
        --clean)        DO_CLEAN=1 ;;
        --build-dir)    BUILD_DIR="$2"; shift ;;
        --prefix)       PREFIX="$2"; shift ;;
        --build-type)   BUILD_TYPE="$2"; shift ;;
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

# Auto-detect stubs vs real Jetson libraries when not forced.
if [ -z "${WITH_STUBS}" ]; then
    if [ -d "${JETSON_API_DIR}" ] && [ -d "${JETSON_LIB_DIR}" ]; then
        WITH_STUBS="OFF"
        echo "[i] Jetson Multimedia API detected -> building against real libraries."
    else
        WITH_STUBS="ON"
        echo "[i] Jetson Multimedia API not found -> building against stubs/ (not runnable)."
    fi
fi

if [ "${DO_CLEAN}" -eq 1 ]; then
    echo "[i] Removing ${BUILD_DIR}"
    # Tolerant: never let cleanup fail the run (root-owned leftovers from a
    # prior sudo --install, missing dir, etc.). Try sudo, then ignore.
    rm -rf "${BUILD_DIR}" 2>/dev/null || sudo rm -rf "${BUILD_DIR}" 2>/dev/null || true
fi

mkdir -p "${BUILD_DIR}"

CMAKE_ARGS=(
    "-DCMAKE_BUILD_TYPE=${BUILD_TYPE}"
    "-DWITH_STUBS=${WITH_STUBS}"
    # Always pass the prefix explicitly so a stale value cached in an existing
    # build dir from a previous --prefix run can never silently win.
    "-DCMAKE_INSTALL_PREFIX=${PREFIX}"
)

echo "[i] Configuring: cmake ${CMAKE_ARGS[*]}"
cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" "${CMAKE_ARGS[@]}"

echo "[i] Building libnvmpi with ${JOBS} jobs"
cmake --build "${BUILD_DIR}" -j "${JOBS}"

# Build .deb package if requested
if [ "${DO_PACKAGE}" -eq 1 ]; then
    echo "[i] Building .deb package (CPack)"
    cmake --build "${BUILD_DIR}" --target package
    echo "[i] Package: $(ls "${BUILD_DIR}"/*.deb 2>/dev/null)"
fi

SUDO=""
[ "$(id -u)" -ne 0 ] && SUDO="sudo"

# Install libnvmpi if requested, or unconditionally when building FFmpeg (the
# FFmpeg build must find libnvmpi via pkg-config and link it at runtime).
if [ "${DO_INSTALL}" -eq 1 ] || [ -n "${FFMPEG_TARGET}" ]; then
    echo "[i] Installing libnvmpi to ${PREFIX}"
    ${SUDO} cmake --install "${BUILD_DIR}"
    ${SUDO} ldconfig || true
fi

if [ -z "${FFMPEG_TARGET}" ]; then
    echo "[i] Done. libnvmpi artifacts in ${BUILD_DIR}"
    exit 0
fi

# ---------------------------------------------------------------------------
# FFmpeg build (--ffmpeg)
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

# Make sure pkg-config and the runtime linker can see the libnvmpi we installed.
export PKG_CONFIG_PATH="${PREFIX}/lib/pkgconfig:${PREFIX}/share/pkgconfig:${PKG_CONFIG_PATH:-}"
export LD_LIBRARY_PATH="${PREFIX}/lib:${JETSON_LIB_DIR}:${LD_LIBRARY_PATH:-}"

"${REPO_ROOT}/scripts/ffpatch.sh" "${FF_DIR}"

FF_CONF=(--enable-nvmpi --disable-doc)
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
