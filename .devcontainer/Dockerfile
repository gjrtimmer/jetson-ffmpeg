# check=skip=InvalidBaseImagePlatform
# Dev container for jetson-ffmpeg
# Runs ON Jetson hardware via VS Code Remote SSH + Dev Containers
#
# Base images by JetPack version:
#   JetPack 5.x (L4T R35): harbor.local/jetson/l4t-jetpack:r35.4.1
#   JetPack 6.0 (L4T R36): harbor.local/jetson/l4t-jetpack:r36.3.0
#   JetPack 6.1 (L4T R36): harbor.local/jetson/l4t-jetpack:r36.4.0
ARG L4T_VERSION=r36.4.0
FROM harbor.local/jetson/l4t-jetpack:${L4T_VERSION}

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    ninja-build \
    pkg-config \
    git \
    gdb \
    strace \
    clang-format \
    libv4l-dev \
    nasm \
    yasm \
    curl \
    wget \
    vim \
    sudo \
    bash-completion \
    libass-dev \
    libfreetype-dev \
    libgnutls28-dev \
    libsdl2-dev \
    libtool \
    libvorbis-dev \
    libx264-dev \
    libxcb1-dev \
    libxcb-shm0-dev \
    libxcb-xfixes0-dev \
    shellcheck \
    texinfo \
    zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

ARG USERNAME=vscode
ARG USER_UID=1000
ARG USER_GID=${USER_UID}

RUN groupadd --gid ${USER_GID} ${USERNAME} 2>/dev/null || true \
    && useradd --uid ${USER_UID} --gid ${USER_GID} -m -s /bin/bash ${USERNAME} 2>/dev/null || true \
    && echo "${USERNAME} ALL=(ALL) NOPASSWD:ALL" >> /etc/sudoers.d/${USERNAME} \
    && chmod 0440 /etc/sudoers.d/${USERNAME}

COPY bashrc /etc/bashrc-custom

# nvm + Node.js LTS
RUN curl -o- https://raw.githubusercontent.com/nvm-sh/nvm/v0.40.5/install.sh | bash \
    && . "${HOME}/.nvm/nvm.sh" \
    && nvm install --lts

# uv / uvx
RUN curl -LsSf https://astral.sh/uv/install.sh | sh

USER ${USERNAME}

RUN NONINTERACTIVE=1 /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

ENV PATH="/home/linuxbrew/.linuxbrew/bin:/home/linuxbrew/.linuxbrew/sbin:${PATH}"

RUN brew install gh glab claude-code

WORKDIR /workspace
