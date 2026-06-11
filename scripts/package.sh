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

# Refresh the dynamic linker cache so libnvmpi.so is found. For a non-standard
# prefix, register its lib dir so the libraries resolve at runtime.
if [ "${PREFIX}" != "/usr/local" ] && [ -w /etc/ld.so.conf.d ] 2>/dev/null; then
    echo "${PREFIX}/lib" > /etc/ld.so.conf.d/jetson-ffmpeg.conf
fi
if command -v ldconfig >/dev/null 2>&1; then ldconfig || true; fi

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
  bin/      ffmpeg, ffprobe (and friends) built with --enable-nvmpi
  lib/      libnvmpi + FFmpeg shared libraries
  include/  headers
  install.sh

Install (on a Jetson):
  tar xzf ${NAME}.tar.gz
  cd ${NAME}
  sudo ./install.sh            # installs into /usr/local
  # or: sudo ./install.sh /opt/jetson-ffmpeg

Verify:
  ffmpeg -hide_banner -encoders | grep nvmpi

Note: the nvmpi codecs require real Jetson hardware and the NVIDIA tegra
libraries (/usr/lib/aarch64-linux-gnu/tegra); there is no software fallback.
EOF

mkdir -p "${OUTDIR}"
TARBALL="${OUTDIR}/${NAME}.tar.gz"
echo "[i] writing ${TARBALL}"
tar -C "${STAGE}" -czf "${TARBALL}" "${NAME}"
rm -rf "${STAGE}"

echo "[i] done: ${TARBALL}"
echo "${TARBALL}"
