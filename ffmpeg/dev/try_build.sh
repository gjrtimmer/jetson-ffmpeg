#!/bin/bash
# Build-validate every supported FFmpeg version.
#
# Regenerates the patches (update_patch.sh), then runs
# ./configure --enable-nvmpi && make in each cloned FFmpeg tree.
#
# Safe to run from any working directory.
set -u

DEV_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # <repo>/ffmpeg/dev

VERSIONS="4.2 4.4 6.0 6.1 7.0 7.1 8.0"

# (Re)generate patches and clone the FFmpeg trees.
"${DEV_DIR}/update_patch.sh"

for ver in ${VERSIONS}; do
    src="${DEV_DIR}/ffmpeg${ver}"
    echo "=== building ffmpeg${ver} ==="
    if ( cd "${src}" && ./configure --enable-nvmpi && make -j"$(nproc)" ); then
        echo "ffmpeg${ver} BUILD OK"
    else
        echo "ffmpeg${ver} BUILD FAIL"
        exit 1
    fi
done
