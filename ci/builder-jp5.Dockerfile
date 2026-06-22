# CI builder image for JetPack 5.x compile-testing.
#
# Installs only the build dependencies needed to compile libnvmpi against JP5
# (kernel 5.10) V4L2 and Multimedia API headers. This is a compile-test-only
# image — no FFmpeg libraries, no CUDA, no runtime deps.
#
# Catches header compatibility regressions (e.g. V4L2 constants added in
# kernel 5.17 that don't exist on JP5's kernel 5.10) before they reach users.
#
# Rebuild when build deps change or when upgrading the JP5 L4T baseline.
ARG L4T_TAG=r35.4.1
FROM harbor.local/jetson/l4t-jetpack5:${L4T_TAG}

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        git \
        build-essential \
        cmake \
        pkg-config \
        libv4l-dev \
    && if [ ! -d /usr/src/jetson_multimedia_api ]; then \
        apt-get install -y --no-install-recommends nvidia-l4t-jetson-multimedia-api; \
    fi \
    && rm -rf /var/lib/apt/lists/*
