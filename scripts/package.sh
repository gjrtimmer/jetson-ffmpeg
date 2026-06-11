#!/usr/bin/env bash
# Package a built libnvmpi + FFmpeg install prefix into a distributable archive
# with a self-contained install.sh.
#
# Given an install PREFIX that already contains the FFmpeg build AND libnvmpi
# (as the CI patch stage produces in _ffmpeg/), this stages bin/lib/include/share
# into a versioned top-level folder, generates an install.sh + README, and emits
#   <outdir>/jetson-ffmpeg_<version>_ffmpeg<ffver>_l4t<l4t>_<arch>.tar.gz
#
# The end user then: tar xzf <archive> && cd <dir> && sudo ./install.sh
#
# Usage:
#   scripts/package.sh --prefix DIR --version VER --ffmpeg-version X [options]
#
#   --prefix DIR        Installed prefix containing bin/ lib/ include/ (required)
#   --version VER       Release / nvmpi version, e.g. 1.1.0 (required)
#   --ffmpeg-version X  FFmpeg version label, e.g. 7.1 (required)
#   --l4t TAG           L4T/JetPack tag (default: $L4T_TAG or r36.4.0)
#   --arch ARCH         Architecture label (default: aarch64)
#   --outdir DIR        Output directory (default: <repo>/dist)
#   -h, --help          Show this help
#
# Safe to run from any working directory.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

PREFIX=""
VERSION=""
FFVER=""
L4T="${L4T_TAG:-r36.4.0}"
ARCH="aarch64"
OUTDIR="${REPO_ROOT}/dist"

usage() { sed -n '2,/^set /{/^set /d;s/^# \{0,1\}//;p}' "${BASH_SOURCE[0]}"; }

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix)         PREFIX="$2"; shift ;;
        --version)        VERSION="$2"; shift ;;
        --ffmpeg-version) FFVER="$2"; shift ;;
        --l4t)            L4T="$2"; shift ;;
        --arch)           ARCH="$2"; shift ;;
        --outdir)         OUTDIR="$2"; shift ;;
        -h|--help)        usage; exit 0 ;;
        *) echo "[E] unknown option: $1" >&2; usage; exit 1 ;;
    esac
    shift
done

for req in PREFIX VERSION FFVER; do
    if [ -z "${!req}" ]; then echo "[E] --${req,,} is required" >&2; usage; exit 1; fi
done
[ -d "${PREFIX}" ] || { echo "[E] prefix not found: ${PREFIX}" >&2; exit 1; }

NAME="jetson-ffmpeg_${VERSION}_ffmpeg${FFVER}_l4t${L4T}_${ARCH}"
STAGE="$(mktemp -d)"
DEST="${STAGE}/${NAME}"
mkdir -p "${DEST}"

echo "[i] staging ${PREFIX} -> ${NAME}"
# Copy the runtime payload (whichever of these exist in the prefix).
for sub in bin lib include share; do
    [ -d "${PREFIX}/${sub}" ] && cp -a "${PREFIX}/${sub}" "${DEST}/"
done

# --- bundle the binaries' external shared-lib dependencies ----------------
# Offline fallback for install.sh: collect every lib the ffmpeg/ffprobe binaries
# link EXCEPT (a) the device-provided tegra/cuda libs, (b) the glibc/loader core,
# and (c) our own libs already in lib/. On install, apt is preferred; whatever
# is still missing is filled from here (so an apt-less Jetson still works).
echo "[i] collecting bundled fallback libs"
mkdir -p "${DEST}/bundled-libs"
export LD_LIBRARY_PATH="${PREFIX}/lib:/usr/lib/aarch64-linux-gnu/tegra:${LD_LIBRARY_PATH:-}"
for bin in "${DEST}"/bin/*; do
    [ -x "$bin" ] || continue
    ldd "$bin" 2>/dev/null | awk '/=>/ && $3 ~ /^\// {print $1" "$3}'
done | sort -u | while read -r soname path; do
    [ -f "$path" ] || continue
    case "$path" in *tegra*|*/cuda*) continue ;; esac
    case "$soname" in
        ld-linux*|libc.so*|libm.so*|libdl.so*|libpthread.so*|librt.so*|libresolv.so*|linux-vdso*) continue ;;
        libnvmpi*|libav*|libsw*|libpostproc*) continue ;;   # ours; already in lib/
    esac
    cp -Ln "$path" "${DEST}/bundled-libs/${soname}" 2>/dev/null || true
done
echo "[i] bundled $(find "${DEST}/bundled-libs" -type f | wc -l) fallback libs"

# --- generated install.sh -------------------------------------------------
cat > "${DEST}/install.sh" <<'INSTALL_EOF'
#!/usr/bin/env bash
# Install this jetson-ffmpeg bundle (libnvmpi + FFmpeg with nvmpi) on a Jetson.
#
# Usage:  sudo ./install.sh [PREFIX]      (PREFIX default: /usr/local)
#         sudo PREFIX=/opt/ff ./install.sh
#
# Requires a real Jetson with the NVIDIA tegra libraries present
# (/usr/lib/aarch64-linux-gnu/tegra) — the nvmpi codecs are hardware-only.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREFIX="${1:-${PREFIX:-/usr/local}}"

echo "[i] installing into ${PREFIX}"
for sub in bin lib include share; do
    [ -d "${HERE}/${sub}" ] && cp -a "${HERE}/${sub}/." "${PREFIX}/${sub}/"
done

# Register the lib dir for a non-standard prefix so libraries resolve at runtime.
if [ "${PREFIX}" != "/usr/local" ] && [ -w /etc/ld.so.conf.d ] 2>/dev/null; then
    echo "${PREFIX}/lib" > /etc/ld.so.conf.d/jetson-ffmpeg.conf
fi
if command -v ldconfig >/dev/null 2>&1; then ldconfig || true; fi

export LD_LIBRARY_PATH="${PREFIX}/lib:/usr/lib/aarch64-linux-gnu/tegra:${LD_LIBRARY_PATH:-}"

# Runtime codec deps (libx264/x265/vpx/opus/mp3lame/vorbis/dav1d/ass/freetype, …).
# Prefer the system package manager; whatever is still missing afterwards is
# filled from the bundled fallback libs, so an apt-less Jetson also works offline.
if command -v apt-get >/dev/null 2>&1; then
    echo "[i] installing runtime codec libs via apt-get"
    apt-get update -qq || true
    apt-get install -y -qq --no-install-recommends \
        libx264-163 libx265-199 libvpx7 libopus0 libmp3lame0 libvorbis0a libvorbisenc2 \
        libdav1d5 libass9 libfreetype6 libnuma1 libv4l-0 2>/dev/null || true
    command -v ldconfig >/dev/null 2>&1 && ldconfig || true
fi
if [ -d "${HERE}/bundled-libs" ]; then
    missing=$(ldd "${PREFIX}/bin/ffmpeg" 2>/dev/null | awk '/not found/{print $1}')
    if [ -n "${missing}" ]; then
        echo "[i] filling missing libs from bundle:" ${missing}
        for so in ${missing}; do
            [ -f "${HERE}/bundled-libs/${so}" ] && cp -a "${HERE}/bundled-libs/${so}" "${PREFIX}/lib/"
        done
        command -v ldconfig >/dev/null 2>&1 && ldconfig || true
    fi
fi

echo "[i] installed. Verifying nvmpi codecs..."
export LD_LIBRARY_PATH="${PREFIX}/lib:/usr/lib/aarch64-linux-gnu/tegra:${LD_LIBRARY_PATH:-}"
if "${PREFIX}/bin/ffmpeg" -hide_banner -encoders 2>/dev/null | grep -q nvmpi; then
    echo "[OK] nvmpi encoders present:"
    "${PREFIX}/bin/ffmpeg" -hide_banner -encoders 2>/dev/null | grep nvmpi || true
    echo "[OK] install complete. 'ffmpeg' is in ${PREFIX}/bin."
    [ "${PREFIX}" != "/usr/local" ] && echo "    (add ${PREFIX}/bin to PATH and ${PREFIX}/lib to the library path)"
else
    echo "[!] Could not confirm nvmpi codecs. Ensure you are on real Jetson"
    echo "    hardware with the tegra libs at /usr/lib/aarch64-linux-gnu/tegra."
fi
INSTALL_EOF
chmod +x "${DEST}/install.sh"

# --- README ---------------------------------------------------------------
cat > "${DEST}/README.txt" <<EOF
jetson-ffmpeg ${VERSION} — FFmpeg ${FFVER} with NVIDIA Jetson nvmpi hardware codecs
L4T/JetPack: ${L4T}   Arch: ${ARCH}

Contents:
  bin/           ffmpeg, ffprobe (and friends) built with --enable-nvmpi
  lib/           libnvmpi + FFmpeg shared libraries
  include/       headers
  bundled-libs/  offline fallback copies of the external codec libs
  install.sh

Install (on a Jetson):
  tar xzf ${NAME}.tar.gz
  cd ${NAME}
  sudo ./install.sh            # installs into /usr/local
  # or: sudo ./install.sh /opt/jetson-ffmpeg

install.sh installs the runtime codec deps (libx264/x265/vpx/opus/…) via apt-get
when it is available, and fills anything still missing from bundled-libs/ — so it
also works on an apt-less / offline system. Stub libraries are intentionally NOT
shipped (they are non-functional placeholders for off-Jetson compilation only).

Verify:
  ffmpeg -hide_banner -encoders | grep nvmpi

Note: the nvmpi codecs require real Jetson hardware and the NVIDIA tegra
libraries (/usr/lib/aarch64-linux-gnu/tegra); there is no software fallback.
EOF

mkdir -p "${OUTDIR}"
TARBALL="${OUTDIR}/${NAME}.tar.gz"
echo "[i] writing ${TARBALL}"
tar -C "${STAGE}" -czf "${TARBALL}" "${NAME}"
# Cleanup must never fail the job once the archive is written.
rm -rf "${STAGE}" 2>/dev/null || sudo rm -rf "${STAGE}" 2>/dev/null || true

echo "[i] done: ${TARBALL}"
echo "${TARBALL}"
