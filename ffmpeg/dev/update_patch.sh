#!/bin/bash
# Regenerate the committed FFmpeg patches.
#
# Clones each supported FFmpeg release, applies the runtime patcher
# (scripts/ffpatch.sh), and writes the resulting git diff to
# ffmpeg/patches/ffmpeg<ver>_nvmpi.patch.
#
# Safe to run from any working directory.
set -eu

DEV_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # <repo>/ffmpeg/dev
REPO_ROOT="$(cd "${DEV_DIR}/../.." && pwd)"
PATCH_DIR="${REPO_ROOT}/ffmpeg/patches"
FFPATCH="${REPO_ROOT}/scripts/ffpatch.sh"

VERSIONS="4.2 4.4 6.0"

for ver in ${VERSIONS}; do
    src="${DEV_DIR}/ffmpeg${ver}"
    # Shallow-clone only if the tree is not already present.
    if [ ! -d "${src}" ]; then
        git clone git://source.ffmpeg.org/ffmpeg.git -b "release/${ver}" --depth=1 "${src}"
    fi

    # Apply the runtime patch.
    "${FFPATCH}" "${src}"

    # Capture the diff as the committed patch file.
    git -C "${src}" add -A .
    git -C "${src}" diff --cached > "${PATCH_DIR}/ffmpeg${ver}_nvmpi.patch"
    echo "Wrote ${PATCH_DIR}/ffmpeg${ver}_nvmpi.patch"
done
