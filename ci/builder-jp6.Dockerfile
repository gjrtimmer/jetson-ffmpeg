# CI builder image for jetson-ffmpeg.
#
# Pre-installs all build dependencies on top of l4t-jetpack so CI jobs
# skip the ~30s apt-get install on every run. Pushed to Harbor and DockerHub;
# pulled in CI from harbor.local/jetson for local caching.
#
# Rebuild when build deps change or when upgrading L4T/JetPack.
ARG L4T_TAG=r36.4.0
FROM harbor.local/jetson/l4t-jetpack:${L4T_TAG}

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        git \
        build-essential \
        cmake \
        pkg-config \
        yasm \
        nasm \
        wget \
        ca-certificates \
        libv4l-dev \
        libx264-dev \
        libx265-dev \
        libnuma-dev \
        libvpx-dev \
        libopus-dev \
        libmp3lame-dev \
        libvorbis-dev \
        libdav1d-dev \
        libass-dev \
        libfreetype6-dev \
        libgnutls28-dev \
    && if [ ! -d /usr/src/jetson_multimedia_api ]; then \
        apt-get install -y --no-install-recommends nvidia-l4t-jetson-multimedia-api; \
    fi \
    && rm -rf /var/lib/apt/lists/*
