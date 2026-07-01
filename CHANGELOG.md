# Changelog

## 3.8.0 - 2026-07-01

### Bug Fixes


**ffmpeg**


- OOM guards, DRM_PRIME cleanup, and VIC filter hardening


**nvmpi**


- Decoder lifecycle and capture-loop hardening


**scripts**


- Use vX.Y.Z release naming convention
- Filter non-user-facing commits from release changelog
- Exclude retro scope commits from release changelog


**test**


- Restrict signal detection to standard POSIX signals (1-31)


### Build


**ffmpeg**


- Regenerate patches for bug fix commits


### Features


**skills**


- Add create-release autonomous release workflow
- Add auto-version detection to create-release


### Testing

## 3.7.0 - 2026-07-01

### Bug Fixes


**devcontainer**


- Add -y flag to brew upgrade in postStartCommand


**ffmpeg**


- Add libavcodec 63 version guards for pix_fmts field move
- Pass original NvBufSurface fd through DRM_PRIME format_modifier
- Propagate hw_frames_ctx in VIC scale filter output
- Use original NvBufSurface fd for encoder DRM_PRIME input
- Propagate hw_frames_ctx on DRM_PRIME output frames
- Make VIC scale filter reinit-safe for FFmpeg 7.x
- Add VIC filter passthrough and use MMAP encoder for DRM_PRIME
- Use feature detection for FF_PROFILE compatibility guard


**nvmpi**


- Use GPU compute for VIC filter to avoid Tegra driver deadlock
- Use VIC compute mode instead of GPU for scale filter transforms
- Use internal buffer copy for encoder DMABUF input
- Guard decoder pool release callback against use-after-free


**nvmpi, ffmpeg**


- Use registered surface copy for VIC filter DRM_PRIME input
- Serialize NvBufSurfTransform to prevent Tegra driver deadlock


### Features


**ffmpeg**


- Dlopen lazy-loading for libnvmpi — no link-time dependency
- Add DRM_PRIME output support to all nvmpi decoders
- Add DRM_PRIME input support to all nvmpi encoders
- Add hwcontext_nvmpi for -hwaccel nvmpi support
- Add DRM_PRIME to CUDA zero-copy transfer via EGL interop
- CUDA interop for hwcontext_nvmpi


**nvmpi**


- Add DMA-BUF fd passthrough API for zero-copy decode/encode
- VIC hardware scale filter


**nvmpi, ffmpeg**


- Add VIC hardware scale filter for zero-copy DRM_PRIME scaling


### Testing


**ffmpeg**


- Add VIC scale filter hardware test suite
- Add diagnostic test case for VIC scale filter pipeline
- Add timeout safety for VIC scale filter and hw-all

## 3.6.7 - 2026-06-25

### Bug Fixes


**nvmpi**


- Probe V4L2 device before factory call for errno diagnosis

## 3.6.6 - 2026-06-25

### Bug Fixes


**nvmpi**


- Use unsigned int instead of uint32_t in public header


### Features


**nvmpi**


- Add encoder runtime controls and init-time options

## 3.6.5 - 2026-06-25

### Features


**nvmpi**


- Implement encoder flush callback for mid-stream reset

## 3.6.4 - 2026-06-24

### Bug Fixes


**scripts**


- Suppress HAVE_VISIBILITY warning in FFmpeg builds


### Build


**cmake**


- Fix option() misuse, add JP6 auto-detect, cross-compile sysroot, CPack .deb, pkg-config fix

## 3.6.3 - 2026-06-23

### Bug Fixes


**ci**


- Use if-form in probe() to avoid set -e exit on missing packages
- Make gnutls conditional in dist template via pkg-config probe
- Probe all optional FFmpeg libs in dist template
- Disable build.sh default x264/x265 flags in dist template
- Replace runtime encoder check with build-artifact verification in dist
- Simplify dist verification to libnvmpi.so existence check

## 3.6.1 - 2026-06-22

### Bug Fixes


**nvmpi**


- Guard V4L2 H.264/HEVC level 5.2-6.2 constants with #ifdef
- Fix misleading-indentation warning and bump cmake minimum

## 3.6.0 - 2026-06-22

### Build


**ffmpeg**


- Add generated patch for FFmpeg 8.1


### Features


**ffmpeg**


- Add FFmpeg 8.1 support

## 3.5.0 - 2026-06-21

### Bug Fixes


**ffmpeg**


- Correct encoder EOS drain path in receive_packet


### Build


**ffmpeg**


- Regenerate patches for encoder EOS drain fix

## 3.4.0 - 2026-06-21

### Build


**ffmpeg**


- Wire stream dimensions through decoder wrapper


### Features


**decoder**


- Accept stream dimensions at decoder creation

## 3.2.0 - 2026-06-21

### Build


**ffmpeg**


- Regenerate patches for encoder blocking wait


### Features


**encoder**


- Add blocking wait to nvmpi_encoder_get_packet


### Testing


**encoder**


- Add hw-encoder-blocking suite

## 3.1.0 - 2026-06-20

### Build


**ffmpeg**


- Regenerate patches for level AVOption additions


### Features


**encoder**


- Wire -level through to V4L2 setLevel for H.264/H.265


### Testing


**encoder**


- Add hw-encoder-level suite for -level verification

## 3.0.0 - 2026-06-20

### Bug Fixes


**nvmpi**


- Make TEST_ERROR macro propagate errors instead of ignoring them


### Refactor


**ffmpeg**


- Drop FFmpeg 4.2 and 4.4 support

## 2.10.0 - 2026-06-19

### Bug Fixes


**nvmpi**


- Resolve decoder capture-loop race on FFmpeg 7.0+ threaded model
- Add goto-cleanup error handling in encoder init


### Build


**stubs**


- Add Tegra libjpeg HW accel stubs for NvJpegEncoder


### Features


**nvmpi**


- Hardware MJPEG encoder via NvJPEGEncoder::encodeFromFd


### Testing


**nvmpi**


- Add MJPEG encoder hardware test suite

## 2.9.0 - 2026-06-19

### Bug Fixes


**nvmpi**


- Retry V4L2 device creation on transient failure


### Build


**stubs**


- Add NvJpegDecoder to build and stub for off-Jetson CI


### Features


**nvmpi**


- Add hardware MJPEG decoder via NvJPEGDecoder


### Testing


**decoder**


- Add MJPEG hardware decoder test suite

## 2.8.2 - 2026-06-18

### Bug Fixes


**encoder**


- Use atomics for cross-thread flags, unique_ptr for NvVideoEncoder


**nvmpi**


- Reduce decoder capture thread CPU overhead


### Testing


**encoder**


- Add concurrent multi-stream encode stress suite

## 2.8.1 - 2026-06-17

### Bug Fixes


**ci**


- Per-suite timeout in hw-all.sh and idempotent release creation


**decoder**


- Guard initFramePool against invalid pool size and double-init


### Testing

## 2.8.0 - 2026-06-17

### Bug Fixes


**nvmpi**


- Guard nvmpi_create_decoder against NULL deref on V4L2 failure


**test**


- Suppress NVIDIA stdout in latency measurement functions
- Replace flawed latency comparison with completion check
- Suppress unused variable warnings in test_bufpool
- Lower decoder-flush seek-to-4s threshold from 10 to 5 frames


### Build


**ffmpeg**


- Regenerate patches for decoder NULL-deref fix


### Features


**ffmpeg**


- Add wait_timeout AVOption for low-delay decoder


**nvmpi**


- Add condition-variable blocking dequeue to NVMPI_bufPool
- Make ctx->eos atomic and add wait_timeout_ms
- Wire shutdown() to capture-loop exit and use atomic eos
- Implement blocking wait in nvmpi_decoder_get_frame()
- Blocking wait in decoder get_frame


### Refactor


**nvmpi**


- Extract nvmpictx struct to nvmpi_dec_internal.h
- Extract capture loop to nvmpi_dec_capture.cpp
- Fix copyNvBufToFrame forward declaration signature
- Extract V4L2 plane setup to nvmpi_dec_planes.cpp
- Rename nvmpi_dec.cpp to nvmpi_dec_api.cpp
- Extract nvmpi_enc_internal.h from nvmpi_enc.cpp
- Extract nvmpi_enc_output.cpp from nvmpi_enc.cpp
- Rename nvmpi_enc.cpp → nvmpi_enc_api.cpp; update CMakeLists
- Modular split of encoder source files
- Rename NVMPI_frameBuf to nvmpi_frame_buffer


**skills**


- Convert vcs-cli skill to a sonnet agent


### Testing


**api**


- Add libnvmpi C API smoke test harness


**decoder**


- Add V4L2 recovery delay before post-stress health check


**encoder**


- Add pool-size boundary and lifecycle stress suites


**nvmpi**


- Add unit tests for NVMPI_bufPool blocking dequeue
- Add hw test suites for blocking wait, lifecycle, and perf

## 2.7.0 - 2026-06-15

### Bug Fixes


**devcontainer**


- Install jetson-stats as root


**test**


- Relax insert_vui assertion for FFmpeg 6.1+ tick rate


### Build


### Features


**nvmpi,ffmpeg**


- Pixel formats — YUVJ420P, NV12 I/O, P010, insert_vui


### Testing


**ffmpeg**


- Hw-format-pixfmt suite for pixel format features

## 2.6.0 - 2026-06-14

### Bug Fixes


**ffmpeg**


- Remove unsafe ff_get_buffer call from flush callback


### Features


**nvmpi**


- Implement decoder flush for seek / stream restart

## 2.5.0 - 2026-06-14

### Performance


**nvmpi**


- Enable max-perf mode, optional DPB disable and poc-type

## 2.4.0 - 2026-06-13

### Bug Fixes


**nvmpi**


- Close GitHub issue for glibc>=2.34 pthread_join segfault
- Fix decoder crash on close — 7 teardown bugs

## 2.3.0 - 2026-06-13

### Bug Fixes


**build**


- Guard pthread_join in NvV4l2ElementPlane for glibc>=2.34


**ci**


- Restrict image builds to scheduled pipeline only
- Set docker CLI image on component jobs, always pull builder


**test**


- Relax decoder-codecs frame threshold to 80%


### Performance


**ci**


- Use lighter l4t-jetpack image for hw-test jobs


### Refactor


**ci**


- Use docker component for image builds


### Testing

## 2.2.0 - 2026-06-12

### Bug Fixes


**caveman**


- Upstream config


**devcontainer**


- Order of install due to permissions
- Bashrc
- Add --yes flag to brew install and include jq


**ffmpeg-dev**


- Never return EAGAIN from the decode callback
- Reference decode-callback EAGAIN abort issue


**nvmpi**


- Bound oversized packets and add configurable chunk_size


**scripts**


- Update
- Update
- Move uv/uvx from devcontainer to user space
- Skills install


### Build


### Features


**devcontainer**


- Add nvm /node / uv / uvx
- Add shellcheck
- Pass envvars

## 2.1.0 - 2026-06-12

### Bug Fixes


**ffmpeg-dev**


- Prime decoder with out-of-band extradata at init


### Build


**nvmpi**


- Make libnvmpi build warning-free under -Wall -Wextra


### Testing


**hw-test**


- Add RTP decode cases for out-of-band parameter sets

## 2.0.0 - 2026-06-11

### Bug Fixes


**devcontainer**


- Mount host tegra libs, CUDA, and multimedia API
- Remove postCreateCommand
- Use .bash_aliases for PS1 prompt
- Fix PS1 prompt sourcing
- Fix PS1 by appending to .bashrc
- Use PROMPT_COMMAND for PS1
- Build .bashrc as root before USER switch
- Set bash as default shell
- Install libx264-dev and drop stale branch step


**scripts**


- Always pass explicit CMAKE_INSTALL_PREFIX in build.sh
- Pass --prefix= (equals form) to ffmpeg configure


**vscode**


- Remove redundant twxs.cmake extension


### Features


**devcontainer**


- Add development extensions
- Add development tools and FFmpeg build deps
- Add claude-code, docker, and cpptools-extension-pack
- Add PS1 prompt with hostname and git branch
- Add Homebrew with gh and glab CLI tools
- Add Claude Code CLI via Homebrew
- Persist home directory with named volume
- Add project command aliases


**ffmpeg**


- Ship committed patches for all 7 versions, regenerated


**github**


- Add runner token request issue templates


**gitlab**


- Add runner token request issue template for GitHub


**release**


- Add packaging script and bump nvmpi to 1.1.0
- Tag-driven GitLab+GitHub releases with per-version archives
- Self-contained archives (apt-or-bundle deps) + harden cleanup rm
- Install.sh auto-elevates with sudo


**scripts**


- Add build.sh for libnvmpi
- Add --ffmpeg option to build.sh


**vscode**


- Configure workspace extension recommendations
- Add claude-code and cpptools-extension-pack
- Add editor settings and editorconfig


### Refactor


**ffmpeg**


- Move ffmpeg_dev and ffmpeg_patches under ffmpeg/


**ffmpeg-dev**


- Make patch scripts path-independent


**ffpatch**


- Resolve paths from script location


**scripts**


- Move ffpatch.sh into scripts/


### Testing

