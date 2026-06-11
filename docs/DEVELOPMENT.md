# Development Guide

This document covers the architecture, build process, and patch management workflow for **jetson-ffmpeg** — a library that enables hardware-accelerated video encoding/decoding on NVIDIA Jetson platforms via FFmpeg's codec interface.

The goal is to ensure that anyone can continue development, update patches for new FFmpeg versions, and maintain the project independently.

---

## Table of Contents

- [Dev Container](#dev-container)
- [Repository Structure](#repository-structure)
- [Architecture Overview](#architecture-overview)
- [The nvmpi Library (libnvmpi)](#the-nvmpi-library-libnvmpi)
- [FFmpeg Integration Layer](#ffmpeg-integration-layer)
- [Patch System](#patch-system)
- [Development Workflow](#development-workflow)
  - [Building libnvmpi](#building-libnvmpi)
  - [Updating FFmpeg Patches](#updating-ffmpeg-patches)
  - [Adding Support for a New FFmpeg Version](#adding-support-for-a-new-ffmpeg-version)
  - [Testing Patches Locally](#testing-patches-locally)
- [Cross-Building with Stubs](#cross-building-with-stubs)
- [Codec Registration Reference](#codec-registration-reference)
- [Version Compatibility Notes](#version-compatibility-notes)
- [Troubleshooting](#troubleshooting)

---

## Dev Container

A containerized development environment is available for working directly on Jetson hardware via VS Code Remote SSH. The container uses NVIDIA's L4T JetPack base image with GPU access through the NVIDIA container runtime.

See [DEVCONTAINER.md](DEVCONTAINER.md) for setup instructions covering host prerequisites, VS Code configuration, and JetPack version selection.

---

## Repository Structure

```
jetson-ffmpeg/
├── CMakeLists.txt              # Build system for libnvmpi (shared + static)
├── nvmpi.pc.in                 # pkg-config template
├── scripts/                    # Operator scripts (run from any directory)
│   ├── build.sh                # Build/install libnvmpi (auto-detects stubs vs Jetson)
│   └── ffpatch.sh              # Patches a vanilla FFmpeg source tree with nvmpi support
├── include/
│   ├── nvmpi.h                 # Public C API (encoder/decoder create/put/get/close)
│   ├── NVMPI_bufPool.hpp       # Thread-safe buffer pool (template)
│   ├── NVMPI_frameBuf.hpp      # DMA frame buffer wrapper
│   └── nvUtils2NvBuf.h         # Compatibility shim: NvUtils API ↔ legacy nvbuf_utils
├── src/
│   ├── nvmpi_dec.cpp           # Decoder: V4L2 → DMA → frame pool → user
│   ├── nvmpi_enc.cpp           # Encoder: frame → V4L2 → packet pool → user
│   └── NVMPI_frameBuf.cpp      # DMA buffer alloc/destroy
├── stubs/                      # Stub .so files for cross-compilation (aarch64)
├── ffmpeg/                     # FFmpeg integration layer
│   ├── dev/                    # Patch development environment
│   │   ├── common/             # FFmpeg codec source files (shared across versions)
│   │   │   └── libavcodec/
│   │   │       ├── nvmpi_dec.c # FFmpeg decoder codec (AVCodec/FFCodec wrapper)
│   │   │       └── nvmpi_enc.c # FFmpeg encoder codec (AVCodec/FFCodec wrapper)
│   │   ├── 4.2/                # Version-specific FFmpeg overlay files
│   │   │   ├── configure       # Patched configure (adds --enable-nvmpi)
│   │   │   ├── libavcodec/Makefile     # Patched Makefile (adds nvmpi_dec.o / nvmpi_enc.o)
│   │   │   └── libavcodec/allcodecs.c  # Patched codec registration
│   │   ├── 4.4/                # Same structure as 4.2
│   │   ├── 6.0/                # Same structure as 4.2
│   │   ├── .gitignore          # Ignores cloned ffmpeg dirs (ffmpeg4.2/, etc.)
│   │   ├── copy_files.sh       # Copies overlay files into cloned FFmpeg trees
│   │   ├── update_patch.sh     # Clones FFmpeg, patches, generates .patch files
│   │   └── try_build.sh        # Runs update_patch.sh then builds all versions
│   └── patches/                # Generated patch files (applied by users)
│       ├── ffmpeg4.2_nvmpi.patch
│       ├── ffmpeg4.4_nvmpi.patch
│       └── ffmpeg6.0_nvmpi.patch
├── test/
│   └── hw-test.sh              # CI smoke test for hardware encode/decode
└── docs/                       # Documentation
```

---

## Architecture Overview

The project has two distinct layers:

```
┌─────────────────────────────────────────────────────┐
│                    FFmpeg Process                     │
│                                                       │
│  ┌─────────────────────────────────────────────────┐ │
│  │ FFmpeg Integration Layer (patched into FFmpeg)   │ │
│  │   nvmpi_enc.c  ←→  AVCodec / FFCodec interface  │ │
│  │   nvmpi_dec.c  ←→  AVCodec / FFCodec interface  │ │
│  └──────────────────────┬──────────────────────────┘ │
│                         │ calls libnvmpi C API        │
│  ┌──────────────────────▼──────────────────────────┐ │
│  │ libnvmpi (shared library, installed on system)  │ │
│  │   nvmpi_dec.cpp   (V4L2 decoder)                │ │
│  │   nvmpi_enc.cpp   (V4L2 encoder)                │ │
│  │   NVMPI_frameBuf  (DMA buffer management)       │ │
│  └──────────────────────┬──────────────────────────┘ │
│                         │ V4L2 / NvBuffer API         │
│  ┌──────────────────────▼──────────────────────────┐ │
│  │ NVIDIA Jetson Multimedia API (system libraries)  │ │
│  │   libnvv4l2, libnvbufsurface, libnvjpeg, etc.   │ │
│  └─────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────┘
```

**Layer 1 — libnvmpi** (`src/`, `include/`, `CMakeLists.txt`): A standalone shared library that wraps NVIDIA's V4L2-based multimedia API into a simple C interface. This is what gets installed on the system (`libnvmpi.so`).

**Layer 2 — FFmpeg integration** (`ffmpeg/dev/common/`, patches): Source files that implement FFmpeg's codec interface (`AVCodec`/`FFCodec`) by calling libnvmpi. These get patched into FFmpeg source trees and compiled as part of FFmpeg.

---

## The nvmpi Library (libnvmpi)

### Public API (`include/nvmpi.h`)

The library exposes a pure C API:

**Decoder:**
- `nvmpi_create_decoder(nvDecParam*)` — Create decoder context
- `nvmpi_decoder_put_packet(ctx, nvPacket*)` — Feed compressed packet
- `nvmpi_decoder_get_frame(ctx, nvFrame*, wait)` — Retrieve decoded frame
- `nvmpi_decoder_close(ctx)` — Destroy decoder

**Encoder:**
- `nvmpi_create_encoder(nvEncParam*)` — Create encoder context
- `nvmpi_encoder_put_frame(ctx, nvFrame*)` — Feed raw frame
- `nvmpi_encoder_get_packet(ctx, nvPacket**)` — Retrieve encoded packet
- `nvmpi_encoder_dqEmptyPacket(ctx, nvPacket**)` — Get empty packet buffer from pool
- `nvmpi_encoder_qEmptyPacket(ctx, nvPacket*)` — Return empty packet buffer to pool
- `nvmpi_encoder_close(ctx)` — Destroy encoder

### Supported Codecs

| Operation | H.264 | H.265/HEVC | VP8 | VP9 | MPEG2 | MPEG4 |
|-----------|-------|------------|-----|-----|-------|-------|
| Decode    | Yes   | Yes        | Yes | Yes | Yes   | Yes   |
| Encode    | Yes   | Yes        | No  | No  | No    | No    |

### Internal Components

- **`NVMPI_bufPool<T>`** (`include/NVMPI_bufPool.hpp`): Thread-safe producer/consumer buffer pool using mutex-guarded queues. Used for both frame buffers (decoder) and packet buffers (encoder).

- **`NVMPI_frameBuf`** (`include/NVMPI_frameBuf.hpp`, `src/NVMPI_frameBuf.cpp`): Wraps DMA buffer allocation/destruction. Abstracts the difference between NvUtils API (JetPack 5+) and legacy nvbuf_utils API.

- **`nvUtils2NvBuf.h`** (`include/nvUtils2NvBuf.h`): Compile-time compatibility layer. When `WITH_NVUTILS` is defined, maps legacy `NvBuffer*` names to `NvBufSurf*` equivalents. This is what enables support across JetPack versions without `#ifdef` in every function.

### NvUtils vs nvbuf_utils

The codebase supports two NVIDIA buffer management APIs:

| Feature | nvbuf_utils (legacy) | NvUtils / NvBufSurface (JetPack 5+) |
|---------|---------------------|--------------------------------------|
| Header  | `nvbuf_utils.h`     | `nvbufsurface.h`, `NvBufSurface.h`   |
| Detection | No `nvbufsurface.h` | `nvbufsurface.h` exists              |
| CMake flag | (default)        | `-DWITH_NVUTILS` (auto-detected)     |

CMakeLists.txt auto-detects this: if `nvbufsurface.h` exists in the Jetson Multimedia API headers, it enables `WITH_NVUTILS` and links the corresponding libraries.

### Building libnvmpi

**On a Jetson device (native build):**

The quickest path is the helper script (auto-detects real Jetson libs vs stubs):

```bash
./scripts/build.sh --install     # alias in the dev container: build --install
```

Equivalent raw CMake build:

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
sudo make install
```

**With custom paths:**

```bash
cmake .. \
  -DJETSON_MULTIMEDIA_API_DIR=/path/to/jetson_multimedia_api \
  -DJETSON_MULTIMEDIA_LIB_DIR=/path/to/tegra/libs \
  -DCUDA_INCLUDE_DIR=/usr/local/cuda/include \
  -DCUDA_LIB_DIR=/usr/local/cuda/lib64
```

**Cross-compilation with stubs:**

```bash
cmake .. -DWITH_STUBS=ON
```

This links against stub libraries in `stubs/` instead of real NVIDIA libraries. Useful for CI/CD or development on non-Jetson hosts. The stub libraries are minimal aarch64 ELF shared objects that satisfy the linker without actual implementations.

### Build outputs

- `libnvmpi.so.1.0.0` (shared library)
- `libnvmpi.a` (static library)
- `nvmpi.pc` (pkg-config file)
- Installed header: `nvmpi.h`

---

## FFmpeg Integration Layer

### How FFmpeg finds nvmpi

FFmpeg's build system needs three modifications to recognize nvmpi as a hardware acceleration library:

1. **`configure`** — Register `nvmpi` in `HWACCEL_LIBRARY_LIST`, add `--enable-nvmpi` flag, define codec dependencies (e.g., `h264_nvmpi_encoder_deps="nvmpi"`)
2. **`libavcodec/Makefile`** — Add object file rules (e.g., `OBJS-$(CONFIG_H264_NVMPI_ENCODER) += nvmpi_enc.o`)
3. **`libavcodec/allcodecs.c`** — Register codec symbols (e.g., `extern FFCodec ff_h264_nvmpi_decoder;`)

### FFmpeg codec source files

Located in `ffmpeg/dev/common/libavcodec/`:

- **`nvmpi_enc.c`** (602 lines): Implements FFmpeg encoder codecs for H.264 and HEVC. Handles profile/level mapping, rate control, packet pool management. Contains version-aware macros for FFmpeg API changes across versions.

- **`nvmpi_dec.c`** (258 lines): Implements FFmpeg decoder codecs for H.264, HEVC, VP8, VP9, MPEG2, MPEG4. Handles frame allocation, DRM prime output, resize options.

Both files use preprocessor version checks to handle FFmpeg API evolution:

```c
// Example: FFCodec vs AVCodec (changed in libavcodec 60+)
#if LIBAVCODEC_VERSION_MAJOR >= 60
#include "codec_internal.h"  // FFCodec
#endif

// Example: FF_PROFILE renamed to AV_PROFILE (FFmpeg 8.0+)
#if (LIBAVCODEC_VERSION_MAJOR >= 62 && LIBAVCODEC_VERSION_MINOR >= 11)
#define FF_PROFILE_H264_INTRA AV_PROFILE_H264_INTRA
#endif
```

### Version-specific overlay files

Each supported FFmpeg version has its own directory under `ffmpeg/dev/`:

```
ffmpeg/dev/4.2/    → configure, libavcodec/Makefile, libavcodec/allcodecs.c
ffmpeg/dev/4.4/    → same files, adjusted for FFmpeg 4.4 differences
ffmpeg/dev/6.0/    → same files, adjusted for FFmpeg 6.0 differences
```

These differ because:
- `configure`: help text, option lists, and codec dependency sections change between versions
- `Makefile`: object file sections and ordering change
- `allcodecs.c`: codec interface changed from `extern AVCodec` (< 60) to `extern const FFCodec` (>= 60)

---

## Patch System

### Overview

The patch system converts the overlay files + common codec sources into standard `git diff` patches that end users apply to vanilla FFmpeg source trees.

### How it works

**`scripts/ffpatch.sh`**: The runtime patching script. Takes a path to an FFmpeg source directory and:

1. Detects `libavcodec` version from headers (`version.h` or `version_major.h`)
2. Creates backup copies of `configure`, `Makefile`, `allcodecs.c`
3. Uses `sed` to insert nvmpi entries into each file:
   - Adds `--enable-nvmpi` to configure help
   - Adds `nvmpi` to `HWACCEL_LIBRARY_LIST`
   - Adds dependency declarations for each codec
   - Adds object file rules to Makefile
   - Adds `extern` declarations to allcodecs.c
4. Copies `nvmpi_dec.c` and `nvmpi_enc.c` into `libavcodec/`
5. Each insertion is idempotent (checks `grep` before `sed`)

**`ffmpeg/dev/update_patch.sh`**: The patch generation script:

1. Shallow-clones FFmpeg release branches (4.2, 4.4, 6.0)
2. Runs `scripts/ffpatch.sh` on each clone
3. Copies overlay files from `ffmpeg/dev/{version}/` and `ffmpeg/dev/common/`
4. Generates patches via `git add -A && git diff --cached`
5. Writes patches to `ffmpeg/patches/`

**`ffmpeg/dev/copy_files.sh`**: Copies overlay + common files into cloned trees (used during development iteration).

**`ffmpeg/dev/try_build.sh`**: Runs `update_patch.sh` then attempts `./configure --enable-nvmpi && make` for each version. Build validation.

### Patch file content

Each patch in `ffmpeg/patches/` is a complete `git diff` that modifies:
- `configure` (nvmpi registration)
- `libavcodec/Makefile` (build rules)
- `libavcodec/allcodecs.c` (codec symbol registration)
- `libavcodec/nvmpi_dec.c` (new file — decoder implementation)
- `libavcodec/nvmpi_enc.c` (new file — encoder implementation)

---

## Development Workflow

### Updating FFmpeg Patches

When you need to modify the FFmpeg codec implementation or fix a bug in the integration layer:

**1. Edit the source files:**

For changes common to all FFmpeg versions:
```bash
# Edit the shared codec files
vim ffmpeg/dev/common/libavcodec/nvmpi_enc.c
vim ffmpeg/dev/common/libavcodec/nvmpi_dec.c
```

For version-specific changes (configure, Makefile, allcodecs.c):
```bash
vim ffmpeg/dev/4.2/configure
vim ffmpeg/dev/4.4/configure
vim ffmpeg/dev/6.0/configure
# (same for Makefile and allcodecs.c)
```

**2. Regenerate patches:**

```bash
cd ffmpeg/dev
./update_patch.sh
```

This will:
- Clone fresh FFmpeg source trees (shallow, into gitignored dirs)
- Apply `scripts/ffpatch.sh` to each
- Copy your edited overlay/common files
- Generate new patch files in `ffmpeg/patches/`

**3. Verify the patches build:**

```bash
cd ffmpeg/dev
./try_build.sh
```

**4. Commit both the source changes and regenerated patches.**

### Adding Support for a New FFmpeg Version

Example: adding FFmpeg 7.0 support.

**Step 1 — Clone and inspect the new version:**

```bash
cd ffmpeg/dev
git clone git://source.ffmpeg.org/ffmpeg.git -b release/7.0 --depth=1 ffmpeg7.0
```

**Step 2 — Create the version overlay directory:**

```bash
mkdir -p 7.0/libavcodec
```

**Step 3 — Copy the closest existing version as starting point:**

```bash
cp 6.0/configure 7.0/configure
cp 6.0/libavcodec/Makefile 7.0/libavcodec/Makefile
cp 6.0/libavcodec/allcodecs.c 7.0/libavcodec/allcodecs.c
```

**Step 4 — Diff the new FFmpeg version against the overlay files:**

Compare the vanilla FFmpeg 7.0 files against your overlay to find what changed:

```bash
diff ffmpeg7.0/configure 7.0/configure
diff ffmpeg7.0/libavcodec/Makefile 7.0/libavcodec/Makefile
diff ffmpeg7.0/libavcodec/allcodecs.c 7.0/libavcodec/allcodecs.c
```

**Step 5 — Update overlay files to match new FFmpeg version:**

The overlay files are essentially copies of the original FFmpeg files with nvmpi entries added. You need to:

- Start from the vanilla FFmpeg 7.0 files
- Add the same nvmpi entries that exist in the 6.0 overlay
- Check if the codec interface changed (e.g., `AVCodec` → `FFCodec` transition happened at version 60)

Key things to check:
- Has `HWACCEL_LIBRARY_LIST` moved or been renamed in configure?
- Has the `allcodecs.c` extern format changed?
- Has the Makefile section where codec objects are listed been restructured?
- Does `libavcodec/version.h` or `version_major.h` report a new major version?

**Step 6 — Check if common codec files need updates:**

Look at `ffmpeg/dev/common/libavcodec/nvmpi_enc.c` and `nvmpi_dec.c` for version checks. If FFmpeg 7.0 introduces API changes (renamed functions, new required fields), add appropriate `#if` guards:

```c
#if LIBAVCODEC_VERSION_MAJOR >= XX
    // new API
#else
    // old API
#endif
```

**Step 7 — Update `scripts/ffpatch.sh`:**

The runtime patching script (`scripts/ffpatch.sh`) uses `sed` with anchors based on existing FFmpeg text. If FFmpeg 7.0 changed the text around insertion points (e.g., the line `--disable-videotoolbox` that anchors `--enable-nvmpi`), update the corresponding `sed` command.

Check each `sed` anchor still exists in the new version:
```bash
grep -n 'disable-videotoolbox' ffmpeg7.0/configure
grep -n 'h264_nvenc_encoder_deps' ffmpeg7.0/configure
grep -n 'CONFIG_H264_CUVID_DECODER' ffmpeg7.0/libavcodec/Makefile
```

**Step 8 — Register the new version with the dev scripts:**

`update_patch.sh`, `copy_files.sh`, and `try_build.sh` each iterate over a
single `VERSIONS` list. Add the new version there:

```bash
# In ffmpeg/dev/update_patch.sh, copy_files.sh, and try_build.sh:
VERSIONS="4.2 4.4 6.0 7.0"
```

The scripts handle cloning, patching (via `scripts/ffpatch.sh`), copying
overlays, and writing `ffmpeg/patches/ffmpeg7.0_nvmpi.patch` automatically —
no per-version code to add.

**Step 9 — (covered by Step 8)**

`try_build.sh` builds every version in the same `VERSIONS` list, so no extra
change is needed.

**Step 10 — Test the full pipeline:**

```bash
cd ffmpeg/dev
./update_patch.sh   # generates patches
./try_build.sh      # builds all versions
```

### Testing Patches Locally

To test a patch against a specific FFmpeg version without the dev scripts:

```bash
# Clone FFmpeg
git clone git://source.ffmpeg.org/ffmpeg.git -b release/6.0 --depth=1 ffmpeg-test
cd ffmpeg-test

# Apply patch
git apply /path/to/jetson-ffmpeg/ffmpeg/patches/ffmpeg6.0_nvmpi.patch

# Build (on Jetson)
./configure --enable-nvmpi
make -j$(nproc)

# Verify codecs registered
./ffmpeg -hide_banner -encoders 2>/dev/null | grep nvmpi
./ffmpeg -hide_banner -decoders 2>/dev/null | grep nvmpi
```

### Hardware Smoke Test

The project includes a CI smoke test (`test/hw-test.sh`) that verifies hardware encode/decode works on actual Jetson hardware:

```bash
JETSON_VARIANT=orin-nano ./test/hw-test.sh
```

It creates a test video with software H.264, transcodes it through h264_nvmpi decode → hevc_nvmpi encode, and verifies the output codec. It then runs three RTP-over-loopback decode cases driven from an SDP file: in-band SPS/PPS (control), out-of-band-only SPS/PPS (regression test for upstream Keylost/jetson-ffmpeg#14 — parameter sets reach the decoder only via SDP `sprop-parameter-sets`, exercising the decoder's extradata priming), and the HEVC equivalent (out-of-band VPS/SPS/PPS).

---

## Cross-Building with Stubs

The `stubs/` directory contains minimal aarch64 ELF shared objects that satisfy the linker:

| Stub | Real library |
|------|-------------|
| `libnvv4l2.so` | NVIDIA V4L2 implementation |
| `libnvjpeg.so` | NVIDIA JPEG library |
| `libnvbufsurface.so` | NvBufSurface API (JetPack 5+) |
| `libnvbufsurftransform.so` | Surface transform (JetPack 5+) |
| `libnvbuf_utils.so` | Legacy buffer utils |
| `libv4l2.so.0` | Symlink to `libnvv4l2.so` |

Use stubs for:
- CI/CD pipelines that verify the library compiles
- Cross-compilation on x86 hosts
- IDE indexing / code completion

Stubs cannot be used for runtime testing — the actual NVIDIA hardware and drivers are required.

---

## Codec Registration Reference

Each nvmpi codec requires entries in three FFmpeg files. This table shows what each codec needs:

### configure entries

| Codec | Dependency line | Bitstream filter select |
|-------|----------------|------------------------|
| `h264_nvmpi` encoder | `h264_nvmpi_encoder_deps="nvmpi"` | — |
| `h264_nvmpi` decoder | `h264_nvmpi_decoder_deps="nvmpi"` | `h264_nvmpi_decoder_select="h264_mp4toannexb_bsf"` |
| `hevc_nvmpi` encoder | `hevc_nvmpi_encoder_deps="nvmpi"` | — |
| `hevc_nvmpi` decoder | `hevc_nvmpi_decoder_deps="nvmpi"` | `hevc_nvmpi_decoder_select="hevc_mp4toannexb_bsf"` |
| `mpeg2_nvmpi` decoder | `mpeg2_nvmpi_decoder_deps="nvmpi"` | — |
| `mpeg4_nvmpi` decoder | `mpeg4_nvmpi_decoder_deps="nvmpi"` | — |
| `vp8_nvmpi` decoder | `vp8_nvmpi_decoder_deps="nvmpi"` | — |
| `vp9_nvmpi` decoder | `vp9_nvmpi_decoder_deps="nvmpi"` | — |

### Makefile entries

Each codec adds one object file rule:
```makefile
OBJS-$(CONFIG_H264_NVMPI_ENCODER)      += nvmpi_enc.o
OBJS-$(CONFIG_H264_NVMPI_DECODER)      += nvmpi_dec.o
# ... etc for each codec
```

### allcodecs.c entries

FFmpeg < 60 (versions 4.2, 4.4):
```c
extern AVCodec ff_h264_nvmpi_decoder;
extern AVCodec ff_h264_nvmpi_encoder;
```

FFmpeg >= 60 (version 6.0+):
```c
extern const FFCodec ff_h264_nvmpi_decoder;
extern const FFCodec ff_h264_nvmpi_encoder;
```

---

## Version Compatibility Notes

### FFmpeg API breakpoints handled in the codebase

| Change | Version | Where handled |
|--------|---------|---------------|
| `ff_alloc_packet2` → `ff_get_encode_buffer` | libavcodec 60+ | `nvmpi_enc.c` |
| `AVCodec` → `FFCodec` (with `codec_internal.h`) | libavcodec 60+ | `nvmpi_enc.c`, `nvmpi_dec.c` |
| New encode API (`encode.h`, `receive_packet`) | libavcodec 58.134+ or 59+ | `nvmpi_enc.c` (`NVMPI_FF_NEW_API`) |
| `FF_PROFILE_*` → `AV_PROFILE_*` | libavcodec 62.11+ | `nvmpi_enc.c` |
| `nvbuf_utils` → `NvBufSurface` API | JetPack 5+ | `nvUtils2NvBuf.h`, `WITH_NVUTILS` define |

### Wrapper code paths by FFmpeg version

Because those guards are compile-time, each supported FFmpeg release compiles
into one of a small number of **distinct wrapper builds** (a "path"). Versions
that share a path compile byte-for-byte identical wrapper code.

| FFmpeg | libavcodec | new encode API | `FFCodec` | `AV_PROFILE` | Path |
|--------|-----------|:---:|:---:|:---:|:---:|
| 4.2 | 58.54  | ✗ | ✗ | ✗ | **A** |
| 4.4 | 58.134 | ✓ | ✗ | ✗ | **B** |
| 6.0 | 60.3   | ✓ | ✓ | ✗ | **C** |
| 6.1 | 60.31  | ✓ | ✓ | ✗ | C |
| 7.0 | 61.3   | ✓ | ✓ | ✗ | C |
| 7.1 | 61.19  | ✓ | ✓ | ✗ | C |
| 8.0 | 62.11  | ✓ | ✓ | ✓ | **D** |

There is no `#if` keyed on libavcodec major 61, so **6.0/6.1/7.0/7.1 all compile
the same wrapper branch** (path C). CI **builds and hw-tests all seven**
versions: the patch stage compiles each (catching FFmpeg-internal breakage
independent of our wrapper), and a per-version `test:hw-ffmpeg-<ver>` job runs
the hardware smoke test on each. The path column above documents *why* several
of those builds are wrapper-identical — useful when triaging a failure (a
path-C-only regression should reproduce across 6.0–7.1, whereas an A/B/D-only
failure points at a version-guarded branch).

### Adding support for future FFmpeg API changes

When FFmpeg deprecates or renames an API:

1. Check the new version's `libavcodec/version.h` or `version_major.h` for the major/minor version
2. Add a preprocessor guard in the common codec files (`ffmpeg/dev/common/libavcodec/`)
3. Test with both old and new versions to ensure the guards work
4. Regenerate patches

---

## Troubleshooting

### `scripts/ffpatch.sh` fails with "Patching ... failed!"

The `sed` commands in `scripts/ffpatch.sh` use anchor strings from FFmpeg source files. If FFmpeg reorganized the file, the anchor may no longer exist. Check which function failed (configure, Makefile, or allcodecs.c) and find the new location of the anchor text in the new FFmpeg version.

### Build fails with undefined reference to `nvmpi_*`

libnvmpi must be installed before building FFmpeg. Ensure `pkg-config --libs nvmpi` returns `-lnvmpi`.

### Codec not found at runtime

1. Verify FFmpeg was configured with `--enable-nvmpi`: `ffmpeg -buildconf | grep nvmpi`
2. Verify libnvmpi is in the library path: `ldconfig -p | grep nvmpi`
3. Verify on actual Jetson hardware (not stubs)

### NvBufSurface errors on JetPack 5+

Ensure the build detected `WITH_NVUTILS`. Check CMake output for "Using NvUtils API." If it says "Using NvBufUtils API." on JetPack 5+, verify the `nvbufsurface.h` header path.

### Encoder runs slowly without max performance mode

The encoder defaults to `max_perf = true`. If you see slow encoding, check that the Jetson power mode is set appropriately: `sudo nvpmodel -m 0` (MAXN).
