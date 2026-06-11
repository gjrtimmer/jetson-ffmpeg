# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

**jetson-ffmpeg** enables hardware-accelerated H.264/HEVC/MPEG2/MPEG4/VP8/VP9 video encode/decode on NVIDIA Jetson via FFmpeg. It is built from **two distinct layers** that ship and build separately:

1. **libnvmpi** (`src/`, `include/`, `CMakeLists.txt`) — a standalone C-API shared library that wraps NVIDIA's V4L2/NvBuffer multimedia API. Installed system-wide as `libnvmpi.so`.
2. **FFmpeg integration** (`ffmpeg_dev/`, `ffmpeg_patches/`) — codec source files (`AVCodec`/`FFCodec` wrappers) that call libnvmpi and get *patched into* a vanilla FFmpeg tree, then compiled as part of FFmpeg.

FFmpeg does not depend on this repo at runtime beyond `libnvmpi.so`; the integration layer is delivered as patches users apply to their own FFmpeg checkout.

## Build & test commands

```bash
# Build libnvmpi on a Jetson (native)
mkdir build && cd build && cmake .. && make -j$(nproc) && sudo make install && sudo ldconfig

# Build off-Jetson / in CI (links against stubs/ instead of real NVIDIA libs — compiles but is NOT runnable)
cmake -DWITH_STUBS=ON .. && make -j$(nproc)

# Patch a vanilla FFmpeg tree (auto-detects version, idempotent)
./ffpatch.sh /path/to/ffmpeg
cd /path/to/ffmpeg && ./configure --enable-nvmpi && make

# Regenerate all patch files after editing the integration layer (clones FFmpeg, patches, diffs)
cd ffmpeg_dev && ./update_patch.sh

# Build-validate every supported FFmpeg version
cd ffmpeg_dev && ./try_build.sh

# Hardware smoke test (requires real Jetson; no software fallback for nvmpi codecs)
JETSON_VARIANT=orin-nano ./test/hw-test.sh
```

There is no unit-test suite — verification is the hardware transcode smoke test (`test/hw-test.sh`) plus CI build/patch jobs (`.github/workflows/ci.yml`, `.gitlab-ci.yml`), which compile against `stubs/` on non-Jetson runners.

## Critical workflow rule: never hand-edit `ffmpeg_patches/`

The files in `ffmpeg_patches/*.patch` are **generated artifacts**. To change the FFmpeg integration:

- Edit the codec implementation in `ffmpeg_dev/common/libavcodec/nvmpi_{enc,dec}.c` (shared across all FFmpeg versions).
- Edit version-specific overlays in `ffmpeg_dev/{4.2,4.4,6.0}/` (`configure`, `libavcodec/Makefile`, `libavcodec/allcodecs.c`) only when a change differs per FFmpeg version.
- Run `ffmpeg_dev/update_patch.sh` to regenerate the patches, then commit **both** the source edits and the regenerated patches.

Two patching mechanisms exist and must stay in sync:
- **`ffpatch.sh`** (repo root) — the *runtime* patcher users run. It uses `sed` against anchor strings in FFmpeg source to insert nvmpi entries. If FFmpeg moves/renames an anchor, these `sed` commands break; failures point at which file's anchor is missing.
- **`ffmpeg_dev/` overlays + `update_patch.sh`** — the *development* path that produces the committed `.patch` files.

## Cross-version compatibility

The codebase supports a wide matrix without per-call `#ifdef` sprawl by concentrating version logic in a few places:

- **FFmpeg API drift** (4.2 → 8.0+): handled with `LIBAVCODEC_VERSION_MAJOR/MINOR` preprocessor guards inside `ffmpeg_dev/common/libavcodec/nvmpi_{enc,dec}.c`. Key breakpoints: `AVCodec`→`FFCodec` (v60), new encode API `receive_packet` (`NVMPI_FF_NEW_API`), `FF_PROFILE_*`→`AV_PROFILE_*` (v62.11). The `allcodecs.c` overlay differs between <60 (`extern AVCodec`) and ≥60 (`extern const FFCodec`) — this is why version overlays exist.
- **JetPack buffer API drift**: legacy `nvbuf_utils` vs newer `NvBufSurface`/NvUtils (JetPack 5+). `CMakeLists.txt` auto-detects by probing for `nvbufsurface.h`; if present it defines `-DWITH_NVUTILS` and links the surface libs. `include/nvUtils2NvBuf.h` is a compile-time shim that maps legacy `NvBuffer*` names to `NvBufSurf*` so the rest of `src/` stays API-agnostic.

When adding a new FFmpeg version or handling a new API change, see the step-by-step guide in `docs/DEVELOPMENT.md` ("Adding Support for a New FFmpeg Version") — it must touch overlays, the common codec files, `ffpatch.sh` anchors, `update_patch.sh`, and `try_build.sh` together.

## libnvmpi internals (`src/`)

- `nvmpi_dec.cpp` / `nvmpi_enc.cpp` — V4L2 decode/encode pipelines exposed through the C API in `include/nvmpi.h` (`nvmpi_create_*`, `put`/`get`, `close`).
- `NVMPI_bufPool.hpp` — thread-safe producer/consumer pool used for both decoded-frame and encoded-packet buffers.
- `NVMPI_frameBuf.{hpp,cpp}` — DMA buffer alloc/destroy, abstracting NvUtils vs nvbuf_utils.

The CMake build also pulls NVIDIA sample classes (`NvVideoDecoder`, `NvVideoEncoder`, etc.) from `${JETSON_MULTIMEDIA_API_DIR}/samples/common/classes` — these are not vendored in this repo and must exist on the build host (or via the devcontainer mounts).

## Further docs

- `docs/BUILD.md` — full build/install, CMake options, verification.
- `docs/DEVELOPMENT.md` — architecture deep-dive, patch system, adding FFmpeg versions, codec registration reference, troubleshooting.
- `docs/DEVCONTAINER.md` — VS Code dev container on Jetson hardware (`.devcontainer/` mounts the host's tegra libs, multimedia API, and CUDA read-only).
