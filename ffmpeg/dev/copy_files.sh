#!/bin/bash
# Copy the version overlays and shared codec sources into the cloned
# FFmpeg trees. Used during development iteration to refresh a checkout
# without regenerating patches.
#
# Safe to run from any working directory.
set -eu

DEV_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # <repo>/ffmpeg/dev

VERSIONS="6.0"

for ver in ${VERSIONS}; do
    src="${DEV_DIR}/ffmpeg${ver}"
    # Version-specific overlay (configure, libavcodec/Makefile, allcodecs.c).
    cp -r "${DEV_DIR}/${ver}/." "${src}/"
    # Shared codec implementation.
    cp -r "${DEV_DIR}/common/." "${src}/"
done
