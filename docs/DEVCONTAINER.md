# Dev Container Setup

This guide covers running the jetson-ffmpeg development environment inside a container on Jetson hardware, accessed via VS Code Remote SSH + Dev Containers.

## Architecture

```
┌──────────────┐      SSH       ┌───────────────────────────────────────┐
│  Local PC    │ ──────────────▸│  Jetson Device                        │
│  (VS Code)   │                │  ┌─────────────────────────────────┐  │
│              │                │  │  Docker (nvidia runtime)        │  │
│  Extensions: │                │  │  ┌───────────────────────────┐  │  │
│  - Remote SSH│                │  │  │  Dev Container            │  │  │
│  - Dev Cont. │                │  │  │  - L4T JetPack base      │  │  │
│              │                │  │  │  - cmake, gcc, gdb       │  │  │
│              │                │  │  │  - GPU access             │  │  │
│              │                │  │  │                           │  │  │
│              │                │  │  │  Host bind mounts (ro):  │  │  │
│              │                │  │  │  - /usr/lib/.../tegra/   │  │  │
│              │                │  │  │  - /usr/src/jetson_mm..  │  │  │
│              │                │  │  │  - /usr/local/cuda/      │  │  │
│              │                │  │  └───────────────────────────┘  │  │
│              │                │  └─────────────────────────────────┘  │
└──────────────┘                └───────────────────────────────────────┘
```

The container bind-mounts three host directories read-only to provide the NVIDIA libraries, Jetson Multimedia API headers/sources, and CUDA toolkit that CMake needs to build libnvmpi.

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
docker run --rm --runtime=nvidia harbor.local/jetson/l4t-jetpack:r36.4.0 nvidia-smi || \
docker run --rm --runtime=nvidia harbor.local/jetson/l4t-jetpack:r36.4.0 cat /proc/device-tree/model
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

### 3. Open Dev Container

1. VS Code detects `.devcontainer/` and prompts: **"Reopen in Container"**
1. Click **Reopen in Container** (or `Ctrl+Shift+P` → **Dev Containers: Rebuild and Reopen in Container**)

First build pulls the L4T JetPack image (~2-5 GB depending on version) and installs dev tools. Subsequent opens use the cached image.

### 4. Build the Project

Once inside the container, CMake auto-configures on first open. To build, use the
`build` alias (provided automatically — see [Command Aliases](#command-aliases)):

```bash
build              # build libnvmpi (auto-detects real Jetson libs vs stubs)
build --install    # build, then install + ldconfig
```

Equivalent raw commands:

```bash
cd /workspace
cmake -B build
cmake --build build -j$(nproc)
```

Or use the CMake sidebar in VS Code (provided by the CMake Tools extension).

### Command Aliases

The container's `~/.bashrc` is assembled from `.devcontainer/bashrc` by the
`postCreateCommand`, so these shortcuts are available in every new container.
Each delegates to a path-independent script, so it works from any directory.

| Alias | Runs | Purpose |
|-------|------|---------|
| `build` | `scripts/build.sh` | Build/install libnvmpi |
| `ffpatch` | `scripts/ffpatch.sh` | Patch a vanilla FFmpeg tree |
| `update-patch` | `ffmpeg/dev/update_patch.sh` | Regenerate the committed patches |
| `try-build` | `ffmpeg/dev/try_build.sh` | Build-validate every FFmpeg version |
| `hw-test` / `test` | `test/hw-test.sh` | Hardware encode/decode smoke test |

> `test` shadows the shell `test` builtin in interactive shells only; scripts
> are unaffected because aliases are not expanded in non-interactive shells.

See `docs/SCRIPTS.md` for full options of each underlying script.

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

Ensure the Jetson can reach the registry and the L4T version matches your JetPack installation. Check available tags at `harbor.local/jetson/l4t-jetpack`.

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

### CMake fails with libraries NOTFOUND

CMake cannot find `nvv4l2`, `nvjpeg`, `nvbufsurface`, or `nvbufsurftransform`. This means the host bind mounts are missing or the paths don't match your JetPack installation.

Verify the host directories exist:

```bash
ls /usr/lib/aarch64-linux-gnu/tegra/libnvv4l2.so
ls /usr/src/jetson_multimedia_api/include/nvbufsurface.h
ls /usr/local/cuda/include/cuda.h
```

If your libraries are at a different path, update the `mounts` array in `.devcontainer/devcontainer.json`.

### No GPU access inside container

1. Verify `--runtime=nvidia` is in `devcontainer.json` `runArgs`
1. Verify NVIDIA runtime works on host: `docker run --rm --runtime=nvidia harbor.local/jetson/l4t-jetpack:r36.4.0 ls /dev/nv*`
1. If using default runtime, remove `--runtime=nvidia` from `runArgs`
