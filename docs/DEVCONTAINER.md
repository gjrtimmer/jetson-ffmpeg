# Dev Container Setup

This guide covers running the jetson-ffmpeg development environment inside a container on Jetson hardware, accessed via VS Code Remote SSH + Dev Containers.

## Architecture

```
┌──────────────┐      SSH       ┌──────────────────────────────────┐
│  Local PC    │ ──────────────▸│  Jetson Device                   │
│  (VS Code)   │                │  ┌────────────────────────────┐  │
│              │                │  │  Docker (nvidia runtime)   │  │
│  Extensions: │                │  │  ┌──────────────────────┐  │  │
│  - Remote SSH│                │  │  │  Dev Container       │  │  │
│  - Dev Cont. │                │  │  │  - L4T JetPack base  │  │  │
│              │                │  │  │  - cmake, gcc, gdb   │  │  │
│              │                │  │  │  - GPU access         │  │  │
│              │                │  │  └──────────────────────┘  │  │
│              │                │  └────────────────────────────┘  │
└──────────────┘                └──────────────────────────────────┘
```

## Prerequisites

### Local Machine

Install these VS Code extensions:

- **Remote - SSH** (`ms-vscode-remote.remote-ssh`)
- **Dev Containers** (`ms-vscode-remote.remote-containers`)

### Jetson Host

The Jetson must have Docker with the NVIDIA container runtime configured.

#### 1. Install Docker

JetPack 6.x typically includes Docker. If not:

```bash
sudo apt-get update
sudo apt-get install -y docker.io
sudo systemctl enable --now docker
sudo usermod -aG docker $USER
```

Log out and back in for group changes to take effect.

#### 2. Install NVIDIA Container Runtime

```bash
sudo apt-get install -y nvidia-container-toolkit
sudo nvidia-ctk runtime configure --runtime=docker
sudo systemctl restart docker
```

#### 3. Verify Setup

```bash
# Docker works without sudo
docker info | grep -i runtime

# NVIDIA runtime available
docker run --rm --runtime=nvidia nvcr.io/nvidia/l4t-base:r36.4.0 nvidia-smi || \
docker run --rm --runtime=nvidia nvcr.io/nvidia/l4t-base:r36.4.0 cat /proc/device-tree/model
```

> **Note:** `nvidia-smi` is not available on all Jetson models. Reading `/proc/device-tree/model` confirms GPU access.

#### 4. Set NVIDIA as Default Runtime (Recommended)

Add to `/etc/docker/daemon.json`:

```json
{
    "runtimes": {
        "nvidia": {
            "path": "nvidia-container-runtime",
            "runtimeArgs": []
        }
    },
    "default-runtime": "nvidia"
}
```

Then restart Docker:

```bash
sudo systemctl restart docker
```

## Usage

### 1. SSH into Jetson from VS Code

1. Open VS Code on your local machine
1. Press `Ctrl+Shift+P` → **Remote-SSH: Connect to Host...**
1. Enter your Jetson's SSH address (e.g., `user@jetson-hostname`)
1. VS Code installs its server component on the Jetson

### 2. Clone the Repository on the Jetson

When connecting to a Jetson via Remote SSH for the first time, there is no workspace on the device. Clone the repository directly on the Jetson before opening the dev container:

1. After connecting via SSH, open a terminal in VS Code (`Ctrl+``)
1. Clone the repository:

   ```bash
   git clone https://github.com/Keylost/jetson-ffmpeg.git ~/jetson-ffmpeg
   ```

1. **File → Open Folder** → select `~/jetson-ffmpeg`

> **Note:** The repository must be cloned on the Jetson itself, not on your local machine. The dev container mounts the workspace from the Jetson's filesystem, so a local clone cannot be used.

### 3. Switch to the Dev Container Branch

The dev container configuration lives on the `feat/devcontainer` branch. Switch to it before opening the container:

1. Open the terminal in VS Code (`Ctrl+``)
1. Run:

   ```bash
   git checkout feat/devcontainer
   ```

### 4. Open Dev Container

1. VS Code detects `.devcontainer/` and prompts: **"Reopen in Container"**
1. Click **Reopen in Container** (or `Ctrl+Shift+P` → **Dev Containers: Rebuild and Reopen in Container**)

First build pulls the L4T JetPack image (~2-5 GB depending on version) and installs dev tools. Subsequent opens use the cached image.

### 5. Build the Project

Once inside the container, CMake auto-configures on first open. To build:

```bash
cd /workspace
cmake -B build
cmake --build build -j$(nproc)
```

Or use the CMake sidebar in VS Code (provided by the CMake Tools extension).

## Configuration

### Changing JetPack Version

Edit `.devcontainer/devcontainer.json`:

```json
"build": {
    "args": {
        "L4T_VERSION": "r35.4.1"
    }
}
```

Common versions:

| JetPack | L4T Version | Jetson Models |
|---------|-------------|---------------|
| 5.1.3   | `r35.5.0`  | AGX Xavier, Xavier NX, Orin NX, Orin Nano |
| 6.0     | `r36.3.0`  | AGX Orin, Orin NX, Orin Nano |
| 6.1     | `r36.4.0`  | AGX Orin, Orin NX, Orin Nano |

After changing, rebuild the container: `Ctrl+Shift+P` → **Dev Containers: Rebuild Container**.

### Adding FFmpeg Build Dependencies

To build FFmpeg inside the container, install additional packages. Add to the `Dockerfile` `apt-get install` line:

```
libass-dev libfreetype-dev libgnutls28-dev libsdl2-dev libtool \
libva-dev libvdpau-dev libvorbis-dev libxcb1-dev libxcb-shm0-dev \
libxcb-xfixes0-dev texinfo wget zlib1g-dev
```

## Troubleshooting

### "nvidia runtime not found"

The NVIDIA container runtime is not installed or not configured. Follow the [Jetson Host](#jetson-host) setup steps.

### Container build fails pulling L4T image

Ensure the Jetson has internet access and the L4T version matches your JetPack installation. Check available tags at `nvcr.io/nvidia/l4t-jetpack`.

### Permission denied on workspace files

The container runs as `vscode` (UID 1000). If your host user has a different UID, rebuild with:

```json
"build": {
    "args": {
        "USER_UID": "1001",
        "USER_GID": "1001"
    }
}
```

### No GPU access inside container

1. Verify `--runtime=nvidia` is in `devcontainer.json` `runArgs`
1. Verify NVIDIA runtime works on host: `docker run --rm --runtime=nvidia nvcr.io/nvidia/l4t-base:r36.4.0 ls /dev/nv*`
1. If using default runtime, remove `--runtime=nvidia` from `runArgs`
