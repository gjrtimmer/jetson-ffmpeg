# CI builder image for JetPack 5.x.
#
# Installs build dependencies on top of l4t-jetpack5 so CI jobs skip the
# ~30s apt-get install on every run. Used for both libnvmpi compile-testing
# and FFmpeg patch builds against the JP5 Multimedia API headers.
#
# Catches header compatibility regressions (e.g. V4L2 constants added in
# kernel 5.17 that don't exist on JP5's kernel 5.10) before they reach users.
#
# Rebuild when build deps change or when upgrading the JP5 L4T baseline.
ARG L4T_TAG=r35.4.1
FROM harbor.local/jetson/l4t-jetpack5:${L4T_TAG}

ARG DEBIAN_FRONTEND=noninteractive

# l4t-jetpack base includes the Multimedia API; add build tools and FFmpeg
# codec dependencies (matching the JP6 builder for parity).
# libdav1d-dev omitted — may not be available on Ubuntu 20.04 (JP5 base);
# the patch template already conditionally enables dav1d via pkg-config.
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
        libass-dev \
        libfreetype6-dev \
        libgnutls28-dev \
    && if [ ! -d /usr/src/jetson_multimedia_api ]; then \
        apt-get install -y --no-install-recommends nvidia-l4t-jetson-multimedia-api; \
    fi \
    && rm -rf /var/lib/apt/lists/*
