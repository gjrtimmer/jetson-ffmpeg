# Build Guide

Complete instructions for building and installing jetson-ffmpeg on NVIDIA Jetson platforms.

---

## Table of Contents

- [Prerequisites](#prerequisites)
- [Step 1: Build and Install libnvmpi](#step-1-build-and-install-libnvmpi)
- [Step 2: Patch and Build FFmpeg](#step-2-patch-and-build-ffmpeg)
- [CMake Options](#cmake-options)
- [Cross-Building with Stubs](#cross-building-with-stubs)
- [Verifying the Installation](#verifying-the-installation)

---

## Prerequisites

- NVIDIA Jetson device (or cross-compilation setup)
- JetPack SDK installed (provides Jetson Multimedia API)
- CMake >= 3.9
- GCC/G++ with C++11 support
- Git

The Jetson Multimedia API headers and libraries are typically installed at:
- Headers: `/usr/src/jetson_multimedia_api`
- Libraries: `/usr/lib/aarch64-linux-gnu/tegra`

---

## Step 1: Build and Install libnvmpi

```bash
git clone https://github.com/Keylost/jetson-ffmpeg.git
cd jetson-ffmpeg
mkdir build
cd build
cmake ..
make
sudo make install
sudo ldconfig
```

This installs:
- `libnvmpi.so` (shared library) to `/usr/local/lib/`
- `libnvmpi.a` (static library) to `/usr/local/lib/`
- `nvmpi.h` (header) to `/usr/local/include/`
- `nvmpi.pc` (pkg-config) to `/usr/local/share/pkgconfig/` and `/usr/local/lib/pkgconfig/`

---

## Step 2: Patch and Build FFmpeg

Clone any supported FFmpeg version (4.2 through 8.0+):

```bash
git clone git://source.ffmpeg.org/ffmpeg.git -b release/7.1 --depth=1
```

Patch FFmpeg with nvmpi support using the `ffpatch.sh` script:

```bash
cd jetson-ffmpeg
./ffpatch.sh ../ffmpeg
```

Build FFmpeg with nvmpi enabled:

```bash
cd ../ffmpeg
./configure --enable-nvmpi
make
sudo make install
```

> **Note:** The `ffpatch.sh` script auto-detects the FFmpeg version from its headers and applies the correct modifications. It works with any FFmpeg version from 4.2 onwards. Add your own `./configure` flags as needed (e.g., `--enable-gpl --enable-libx264`).

---

## CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `JETSON_MULTIMEDIA_API_DIR` | `/usr/src/jetson_multimedia_api` | Path to Jetson Multimedia API headers and common sources |
| `JETSON_MULTIMEDIA_LIB_DIR` | `/usr/lib/aarch64-linux-gnu/tegra` | Path to Jetson Multimedia libraries |
| `CUDA_INCLUDE_DIR` | `/usr/local/cuda/include` | Path to CUDA headers |
| `CUDA_LIB_DIR` | `/usr/local/cuda/lib64` | Path to CUDA libraries |
| `WITH_STUBS` | `OFF` | Link against stub libraries instead of real NVIDIA libraries |

Example with custom paths:

```bash
cmake .. \
  -DJETSON_MULTIMEDIA_API_DIR=/home/user/jetson_multimedia_api \
  -DJETSON_MULTIMEDIA_LIB_DIR=/home/user/tegra_libs \
  -DCUDA_INCLUDE_DIR=/usr/local/cuda-11/include
```

---

## Cross-Building with Stubs

For CI/CD pipelines, Docker images, or development on non-Jetson hosts, build against stub libraries:

```bash
cmake -DWITH_STUBS=ON -DJETSON_MULTIMEDIA_API_DIR=/path/to/jetson_multimedia_api ..
make
```

The `stubs/` directory contains minimal aarch64 ELF shared objects that satisfy the linker. These are not functional at runtime — actual Jetson hardware and drivers are required.

---

## Verifying the Installation

**Check libnvmpi is installed:**

```bash
pkg-config --libs nvmpi
# Expected: -lnvmpi

ldconfig -p | grep nvmpi
# Expected: libnvmpi.so.1 (libc6,...) => /usr/local/lib/libnvmpi.so.1
```

**Check FFmpeg has nvmpi codecs:**

```bash
ffmpeg -hide_banner -encoders 2>/dev/null | grep nvmpi
# Expected: V..... h264_nvmpi ...
# Expected: V..... hevc_nvmpi ...

ffmpeg -hide_banner -decoders 2>/dev/null | grep nvmpi
# Expected: V..... h264_nvmpi ...
# Expected: V..... hevc_nvmpi ...
# Expected: V..... mpeg2_nvmpi ...
# Expected: V..... mpeg4_nvmpi ...
# Expected: V..... vp8_nvmpi ...
# Expected: V..... vp9_nvmpi ...
```

**Check FFmpeg build configuration:**

```bash
ffmpeg -buildconf 2>&1 | grep nvmpi
# Expected: --enable-nvmpi
```

---

## Usage Examples

**Decode H.264 video:**

```bash
ffmpeg -c:v h264_nvmpi -i input.mp4 -f null -
```

**Decode with hardware-accelerated scaling:**

```bash
ffmpeg -c:v h264_nvmpi -resize:v 1920x1080 -i input.mp4 -f null -
```

**Encode to H.264:**

```bash
ffmpeg -i input.mp4 -c:v h264_nvmpi output.mp4
```

**Transcode H.264 to H.265:**

```bash
ffmpeg -c:v h264_nvmpi -i input.mp4 -c:v hevc_nvmpi output.mp4
```
