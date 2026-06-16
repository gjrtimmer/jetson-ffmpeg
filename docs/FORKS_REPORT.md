# Fork Network — Full Analysis Report

Detailed findings from every fork of jocover/jetson-ffmpeg and Keylost/jetson-ffmpeg.
This document preserves all agent findings so nothing is lost between sessions.

**Generated:** 2026-06-12
**Scope:** 237 unique forks, 62 active, 2 upstream repos

---

## Table of Contents

1. [Upstream: jocover/jetson-ffmpeg](#upstream-jocover)
2. [Upstream: Keylost/jetson-ffmpeg](#upstream-keylost)
3. [Fork: dsleep (32 ahead)](#dsleep)
4. [Fork: muxable (28 ahead)](#muxable)
5. [Fork: YuriiHoliuk (26 ahead)](#yuriiholiuk)
6. [Fork: xia-chu (16 ahead)](#xia-chu)
7. [Fork: mattangus (14 ahead)](#mattangus)
8. [Fork: JeanPaulSB (11 ahead)](#jeanpaulsb)
9. [Fork: HunterAP23 (10 ahead)](#hunterap23)
10. [Fork: wangxiaoshuang (9 ahead)](#wangxiaoshuang)
11. [Fork: hsuanguo (8 ahead)](#hsuanguo)
12. [Fork: bradcagle (7 ahead)](#bradcagle)
13. [Fork: fingul (7 ahead)](#fingul)
14. [Fork: mguzzina (6 ahead)](#mguzzina)
15. [Fork: v-faeez-kadiri (5 ahead)](#v-faeez-kadiri)
16. [Fork: spotai (5 ahead)](#spotai)
17. [Fork: izdeveloper (3 ahead)](#izdeveloper)
18. [Fork: madsciencetist (3 ahead)](#madsciencetist)
19. [Fork: w3sip (branch-only)](#w3sip)
20. [Fork: cybernhl/cgutman](#cybernhl)
21. [Small forks + branch-only](#small-forks)
22. [Fork subtrees (douo, vietnx, aveao)](#subtrees)
23. [Master priority list](#priority-list)

---

## 1. Upstream: jocover/jetson-ffmpeg {#upstream-jocover}

**Total issues:** 119 (80 open, 39 closed) — repo effectively abandoned since ~2022
**Total PRs:** 20 (11 open, 7 merged, 2 closed-unmerged)

### Issue Clusters

**1. nvbuf_utils removal / JetPack 5.1.2+ build failure** — #115, #120, #123, #126, #134, #136

- CMake `find_library(LIB_NVBUF nvbuf_utils)` fails on JP 5.1.2+
- Unresolved upstream; PR #125 (douo) migrates decoder only
- Migration guide: <https://developer.nvidia.com/sites/default/files/akamai/embedded/nvbuf_utils_to_nvutils_migration_guide.pdf>

**2. FFmpeg version support 5.x/6.x/7.x** — #129, #138

- Upstream patch only supports 4.2; API changed (AVCodec→FFCodec v60, encode2→receive_packet)
- PR #132 (bmegli) adds new encode API

**3. Encoder latency (old encode2 API)** — #131, #7

- encode2 callback blocks synchronously; 2-5 frame latency
- PR #132 implements send_frame/receive_packet

**4. Encoder thread race condition** — #130

- `packet_pools` accessed by DQ callback + FFmpeg thread without synchronization
- Identified by bmegli; no fix exists

**5. Missing return statement UB** — #127, #98

- Encoder/decoder capture callbacks declared void* but no return
- GCC on newer L4T → segfault
- Fix: <https://github.com/Extend-Robotics/jetson-ffmpeg/commit/0229e5ba> (encoder)
- Fix: <https://github.com/Extend-Robotics/jetson-ffmpeg/commit/6d327624> (decoder)

**6. Encoder PTS truncation + circular buffer** — #41, #48, #100

- PTS % 1000000 causes non-monotonic DTS in FLV/RTMP
- Old packets re-queued with stale timestamps
- Patches: <https://github.com/jocover/jetson-ffmpeg/files/4803889/patches.zip>

**7. YUVJ420P / pixel format issues** — #20, #60, #66, #68, #73, #83, #116

- Decoder hardcodes yuv420p; yuvj420p (JPEG range) causes crashes
- PR #74 (open): treat YUVJ420P same as YUV420P

**8. Linesize/stride mismatch crash** — #64, #80

- Hardware stride ≠ FFmpeg linesize at 4K; image_copy_plane asserts
- PR #79 (vietnx) MERGED — the critical fix

**9. Decoder hangs with IP cameras** — #111, #1, #5, #13

- Certain RTSP streams cause capture loop hang
- PR #22 (cgutman, Moonlight fixes) MERGED; PR #70 (pix_fmt default) MERGED

**10. NV12 pixel format support** — #11, #73, #133

- Only yuv420p exposed; NV12 is native HW format but not available
- PR #25, #26, #27 (xsacha) open+unmerged

**11. Decoder performance — setMaxPerfMode missing** — #120, PR #125

- 4K HEVC: 28fps → 64fps with one line `setMaxPerfMode(1)`
- 720p H264: 46fps → 290fps
- Not merged upstream; implemented in Keylost

**12. Decode in child process** — PR #99

- V4L2 fd inheritance after fork() breaks decoder
- Keylost PR open+unmerged upstream

**13. Memory leaks** — PR #15 (merged), #16 (merged), #82 (open)

- Decoder leaks fixed; encoder leaks unfixed since 2021

**14. Encoder quality control** — #50, #52, #7, #93

- No CRF; bitrate settings misapplied; lossless requires hardcode

**15. Wrong encoder output size / green line** — #8, #101, #51

- Dimensions must be divisible by 16; encoder pads silently

**16. CUDA / zero-copy** — #62, #67, #92, #93, #121

- Decoded frames always copied to CPU; no `-hwaccel cuda` support
- <https://docs.nvidia.com/jetson/l4t-multimedia/l4t_mm_02_video_dec_cuda.html>

**17. Non-monotonic DTS** — #76, #110

- Related to PTS truncation cluster

**18. RTSP looping stream failure** — #113

- Decoder fails on RTSP stream restart; NVIDIA V4L2 limitation

**19. Build issues** — #2, #37, #47, #56, #109

- Various pkg-config / cmake path / patch application problems

**20. VP8 missing definition** — #71, #72

- VP8 codec registration omitted from allcodecs.c

**21. MPEG2 decoder segfault** — #38

- Unresolved; multiple users confirmed crash

**22. High CPU with low GPU utilization** — #18, #35, #42, #93

- NVENC/NVDEC appear in separate jtop counters, not GPU meter
- setMaxPerfMode is real perf fix

**23. Encoder extradata/SPS-PPS issues** — #3, #45

- extradata_size absurdly large sometimes
- teplofizik patch: <https://github.com/jocover/jetson-ffmpeg/files/4804050/ffmpeg_nvmpi_v2.zip>

**24. DMA buffer encode** — #117, #118

- Feature request for direct DMA-buffer-fd encode; unresolved

**25. Thread lifecycle / Moonlight** — #10, #104

- Rapid open/close crashes; GCC >7.3.x thread-join hang
- PR #22, #23 (cgutman) MERGED

**26. NvBuf header version mismatch** — #31, #32

- Repo-vendored nvbuf_utils.h differs from system version

**27. 10-bit video** — #75, #80

- HW supports 10-bit but nvmpi doesn't expose it

**28. License** — #30

- MIT license added by jocover after cgutman request

### Merged PRs

- PR #15: memory leak fixes in decoder
- PR #16: additional memory leak fix
- PR #22: cgutman Moonlight fixes (thread lifecycle, double-free, frame race)
- PR #23: cgutman low-delay decode
- PR #28: allow FFmpeg build without nvmpi
- PR #70: pix_fmt default fallback to yuv420p
- PR #79: vietnx linesize/stride fix

### Key External Links

- NVIDIA NvUtils migration guide: <https://developer.nvidia.com/sites/default/files/akamai/embedded/nvbuf_utils_to_nvutils_migration_guide.pdf>
- NVIDIA setMaxPerfMode docs: <https://docs.nvidia.com/jetson/l4t-multimedia/classNvVideoDecoder.html#a3334a8ebb14385f42dabc62a7dbdadb2>
- NVIDIA decoded frames to CUDA: <https://docs.nvidia.com/jetson/l4t-multimedia/l4t_mm_02_video_dec_cuda.html>
- NVIDIA unified memory on Tegra: <https://docs.nvidia.com/cuda/archive/10.2/cuda-for-tegra-appnote/index.html#memory-selection>
- teplofizik encoder patches: <https://github.com/jocover/jetson-ffmpeg/files/4803889/patches.zip>
- teplofizik extradata fix: <https://github.com/jocover/jetson-ffmpeg/files/4804050/ffmpeg_nvmpi_v2.zip>
- Extend-Robotics missing return fixes: commits 0229e5ba, 6d327624
- LinusCDE/mad-jetson-ffmpeg: combines NVIDIA official decode + jetson-ffmpeg encode
- NVIDIA official decode-only patch: <http://ffmpeg.org/pipermail/ffmpeg-devel/2020-June/263746.html>
- Pre-built Jetson packages: <https://jetson.repo.azka.li>
- Performance: 4K HEVC 28→64fps, 720p H264 46→290fps with setMaxPerfMode

---

## 2. Upstream: Keylost/jetson-ffmpeg {#upstream-keylost}

**Total issues:** 40 (18 open, 22 closed)
**Total PRs:** 21 (12 open, 5 merged, 4 closed-unmerged)

### Issue Clusters

1. **RTSP out-of-band SPS/PPS hang** — #1, #14 — Our fix: gjrtimmer@08e32c15; w3sip@de0b039 partial
2. **JetPack 5.1.2+ nvbuf_utils** — #16, PR#23 — Fixed in master
3. **Encoder bufsize ignored** — #5 — Fixed upstream
4. **FFmpeg version support** — #6, #13, #28, PR#19 — ffpatch.sh universal now
5. **Decode resize alignment** — PR#8→reverted #10 — Fixed in master later
6. **Child process segfault / stubs** — PR#9 — Merged 2024-09-30
7. **WITH_NVUTILS CPU regression** — #11 — Measurement artifact
8. **All frames marked as keyframes** — #26 — **UNFIXED upstream** (spotai has fix)
9. **Segmented HEVC** — #39 — Fixed Aug 2025
10. **Codec flush not implemented** — #42 — **UNFIXED**
11. **Encoder 4K max** — #3, #44 — Hardware limit
12. **Orin Nano no HW encoder** — #43 — Hardware limit
13. **HEVC over RTMP** — #35 — Needs Enhanced RTMP (FFmpeg ≥6.1)
14. **stderr vs stdout** — PR#15 — **UNFIXED** upstream; our PR#53 covers
15. **GPU memory / hwcontext** — #31, #47 — **UNFIXED**; jellyfin-ffmpeg#653
16. **Encoder packet pool EAGAIN** — #38 — Workaround: `-packet_pool_size:v 32`
17. **CPU rising during decode** — #41 — Cannot reproduce on JP6.2
18. **JPEG HW decode/encode** — #37, #40 — **UNFIXED**; NVJPG engine unused
19. **H265 setLevel missing** — #50 — **UNFIXED**
20. **Lossless H265** — #45 — **UNFIXED**; enableLossless exists but not exposed
21. **AV1 support** — #49 — **UNFIXED**; HW capable on AGX Orin
22. **Upstream to FFmpeg mainline** — #46 — No response
23. **Ubuntu 22.04 compilation** — #21, PR#48 — Workaround: downgrade linux-libc-dev
24. **pkg-config nvmpi not found** — #22, #36 — User error; documented

### Key Links

- gjrtimmer/jetson-ffmpeg@08e32c15 — RTSP fix
- w3sip/jetson-ffmpeg@de0b039 — Partial RTSP fix
- jellyfin/jellyfin-ffmpeg#653 — Jellyfin Jetson support
- NVIDIA forum /t/189290/11 — child process segfault root cause (nvjpeg)
- NVIDIA forum /t/298688 — encoder max resolution

---

## 3. dsleep/jetson-ffmpeg (32 ahead, jocover fork) {#dsleep}

Issues: disabled | PRs: none | Branches: master only

### Commits (32)

- Vendor all NVIDIA multimedia API sample classes in-repo (~7000 lines)
- JetPack 4/5 dual-path encoder via `#if JETPACK_VER` (auto-detect via lsb_release)
- Wrong color format fix: `V4L2_PIX_FMT_ABGR32` → `V4L2_PIX_FMT_XRGB32` (`018e6e2e`)
- Encoder hang fix: raw `NvVideoEncoder*` → `unique_ptr`, reduced log_level (`17443b0e`)
- `NVBUF_COLOR_FORMAT_BGRA/BGRx` added to `fill_bytes_per_pixel()`
- `isRawRGBA` param actually wired (`cf1bb455`)
- Separate `nvmpiJP4.pc.in` for JetPack 4

### Value: MEDIUM

- Color format fix and encoder hang fix are portworthy
- Vendoring NVIDIA classes is a different approach than our devcontainer mount
- JP4/5 dual-path is reference material

---

## 4. muxable/jetson-ffmpeg (28 ahead, jocover fork) {#muxable}

Issues: disabled | PRs: 1 merged (#1 version from env var) | Branches: master only

### Key Features

- **Dynamic bitrate control**: `nvmpi_encoder_set_bitrate(ctx, bitrate)` — change bitrate mid-stream
- **Force IDR**: `nvmpi_encoder_force_idr(ctx)` — on-demand keyframe insertion
- `nvFrame.flags` → `nvFrame.idr` (bool) — cleaner IDR signalling
- Massively expanded encoder params: per-frame-type QP, SAR, bit_depth, chroma_format_idc, slice_level_encode, CABAC toggle, VUI insert, AUD insert, lossless, ROI, reconstructed surface
- Cross-compilation via `$TARGET_ROOTFS` sysroot + `aarch64.cmake` toolchain
- GitHub Actions `.deb` build pipeline + CPack packaging
- Pre-built binary distribution in `build.nvidia/`
- `ffmpeg_nvmpi.patch` DELETED — treats libnvmpi as standalone distributable

### Value: HIGH

- Dynamic bitrate + force IDR are essential for real-time streaming
- Expanded encoder params expose hardware capabilities we're missing

---

## 5. YuriiHoliuk/jetson-ffmpeg (26 ahead of Keylost) {#yuriiholiuk}

Issues: 0 | PRs: 0 | Branches: same as Keylost (no extra work on branches)

### THIS IS THE MOST ARCHITECTURALLY SIGNIFICANT FORK

**Phase 1 — dlopen lazy-loading:**

- `b4ede2c9`: dlopen `libnvmpi.so` to prevent EGL crash at startup
- `dynlink_nvmpi.h` header (+186 lines)

**Phase 2 — Zero-copy DMA-BUF pipeline:**

- `nvmpi_decoder_get_frame_fd()` — zero-copy DMA-BUF fd export from decoder
- `nvmpi_encoder_put_frame_fd()` — zero-copy DMA-BUF fd import to encoder
- FFmpeg DRM_PRIME decode output + DMA-BUF encode input
- `-dmabuf_input` encoder option for explicit DMABUF mode
- EOS/flush crash fixes in DMABUF mode
- Multiple iterations fixing fd sync, temp encoder segfault

**Phase 2.3 — VIC hardware scale filter:**

- `vf_scale_vic.c` (+468 lines) — FFmpeg filter using `NvBufSurfTransform` zero-copy
- DRM_PRIME in/out, dlopen `libnvbufsurface.so`
- Usage: `-vf "scale_vic=w=1280:h=720"`

**Phase 3 — P010 10-bit decode:**

- P010 pixel format support in decoder
- `-p010 1` decoder option

### Files Changed (11)

- `dynlink_nvmpi.h` (NEW), `nvmpi_dec.c` (+224/-56), `nvmpi_enc.c` (+68/-26)
- `vf_scale_vic.c` (NEW +468), `ffpatch.sh` (+33/-2)
- `nvmpi.h` (+33/-1), `nvmpi_dec.cpp` (+101/-21), `nvmpi_enc.cpp` (+106/-3)
- `nvmpi_surface.cpp` (NEW +53)

### Value: **CRITICAL** — this is the zero-copy DMA-BUF pipeline the community has been asking for

---

## 6. xia-chu/jetson-ffmpeg (16 ahead, jocover fork) {#xia-chu}

Issues: disabled | PRs: none | Branches: master only

### Key Changes

- **Child process decoder fix** (`fd0900d7`): `NvBufferSession` create/destroy around transform — fixes decoder in forked processes. Via Keylost `fork_fix` branch.
- FFmpeg 4.3 patch compatibility
- Reverted jocover PR#78 (CMake/pc changes considered problematic)
- Patch applicability fix (missing `new file mode`)
- 6 upstream syncs from jocover PRs

### Value: MEDIUM — child process fix is important but already in Keylost

---

## 7. mattangus/jetson-ffmpeg (14 ahead, jocover fork) {#mattangus}

Issues: disabled | PRs: none | Branches: master, feature/docker

### Key Changes

- Cross-compilation Dockerfile (QEMU `multiarch/qemu-user-static`, L4T r32.5.0)
- LF line ending fixes, patch line fix
- 6 upstream merge commits

### Value: LOW — Docker approach is JetPack 4 era only

---

## 8. JeanPaulSB/jetson-ffmpeg (11 ahead, jocover fork) {#jeanpaulsb}

Issues: disabled | PRs: none | Branches: master only

### Key Changes

- Remove `-lnvbufsurface` from `nvmpi.pc.in` Libs line
- 7 commits iterating on patch whitespace/formatting (churn)

### Value: LOW — the pkg-config fix is minor

---

## 9. HunterAP23/jetson-ffmpeg (10 ahead, jocover fork) {#hunterap23}

Issues: disabled | PRs: none | Branches: master only

### Key Changes

- Missing `new file mode 100644` for nvmpi_enc.c in patch — caused `git apply` to fail
- CRLF→LF conversion
- 6 upstream merges

### Value: LOW — patch applicability fix, superseded by our patch system

---

## 10. wangxiaoshuang/jetson-ffmpeg (9 ahead, 6 behind Keylost) {#wangxiaoshuang}

Issues: disabled | PRs: none | Branches: master only

### Key Changes

- dlopen `libnvmpi.so` (independent from YuriiHoliuk)
- Major decoder rewrite in `nvmpi_dec.cpp` (+138/-209)
- API surface reduction in `nvmpi.h` (+11/-44)
- Commit messages mostly in Chinese; limited documentation

### Value: LOW — dlopen is independently done; decoder rewrite is undocumented and diverged

---

## 11. hsuanguo/jetson-ffmpeg (8 ahead, 25 behind Keylost) {#hsuanguo}

Issues: disabled | PRs: 4 merged

### Key Changes

- **RTSP stream stuck fix** (PR#3, co-authored with w3sip): Feed SPS/PPS extradata to decoder
- **JetPack 5.1.2 build fix** (PR#1): CMakeLists + framebuf fixes
- **nvjpeg removal** (PR#2): Drops unused dependency, adds GPL flag
- **FFmpeg 6.1 support** (PR#4): Patch + allcodecs overlay + build script
- Branches: feature/cherry-pick-rtsp-patch, feature/ffmpeg-6.1, feature/fixed-child-thread, feature/minor-fixes (all pre-merge work)

### Value: MEDIUM — RTSP fix important but same root as w3sip/gjrtimmer fix

---

## 12. bradcagle/jetson-ffmpeg (7 ahead, jocover fork) {#bradcagle}

Issues: disabled | PRs: none | Branches: master, nvutils

### Key Changes (master)

- **JPEG decode prototype** (`cd9bf72e`): NvJPEGDecoder C API (`nvmpi_create_jpeg_decoder`, `nvmpi_decode_jpeg`, `nvmpi_close_jpeg_decoder`). Incomplete implementation.
- **YUVJ420P→YUV420P remapping** (`67e7bbcd`): Forces YUVJ to YUV in decoder

### Key Changes (nvutils branch)

- Full NvBufSurface/NvUtils migration: decoder + CMake done, encoder partial
- Dynamic library detection for tegra libs
- `setMaxPerfMode(1)` enabled

### Value: MEDIUM — JPEG prototype is reference for our JPEG feature; NvUtils migration is reference

---

## 13. fingul/jetson-ffmpeg (7 ahead, 25 behind Keylost) {#fingul}

Issues: disabled | PRs: none | Branches: master only

### Key Changes

- JetPack 6.2 / Ubuntu 22.04 / NX Orin one-shot install script (37 lines)
- sed-patches CMakeLists.txt to skip nvbuf_utils

### Value: LOW — install script useful as reference but brute-force approach

---

## 14. mguzzina/jetson-ffmpeg (6 ahead of Keylost) {#mguzzina}

Issues: disabled | PRs: none | Branches: master only

### Key Changes — WIP `hwcontext_nvmpi` implementation

- `hwcontext_nvmpi.c` (NEW +77): registers `ff_hwcontext_type_nvmpi` with YUV420P/NV12/DRM_PRIME
- `hwcontext_nvmpi.h` (NEW +60): `AVNVMPIDRMFrameDescriptor`, `AVNVMPIFramesContext`
- `ffpatch.sh` (+110): integration hooks
- **Context**: Working on Jellyfin support (jellyfin-ffmpeg#653)

### Value: **HIGH** — architecturally the correct approach for FFmpeg GPU pipeline integration

---

## 15. v-faeez-kadiri/jetson-ffmpeg (5 ahead, 25 behind Keylost) {#v-faeez-kadiri}

Issues: none | PRs: 1 merged

### Key Changes

- JetPack 6 library path: `/tegra` → `/nvidia` in all find_library and rpath
- aarch64 include path addition
- nvbuf_utils removal for JP6

### Value: LOW — path migration is straightforward; nhahn's approach (auto-detect) is better

---

## 16. spotai/jetson-ffmpeg (ffmpeg-8-support branch) {#spotai}

Issues: disabled | PRs: none | Master: identical to v-faeez-kadiri

### Key Changes (ffmpeg-8-support branch — 20+ commits)

- **KEY FRAME FLAG FIX** (`1324d7d6`): Was `pkt->flags |= 0x0001` on every packet; now uses `enc_metadata.KeyFrame` — **CRITICAL BUG FIX**
- **H.265 encoder header fix** (`2b715e81`): Extended GLOBAL_HEADER to H.265; bounded IDR NAL scan loop (was unbounded → could scan past buffer)
- **frame_pool_size / packet_pool_size** (`cf0560b5`): Configurable pool sizes + decoder no-skip-frames fix
- **FFmpeg 8.0+ compat** (`f0a52dfa`)
- **Initial dmabuf support** (`854599a1`)
- nvbufsurface search path fix, CMake custom dirs, nvjpeg stub
- Tegra library stubs, ffpatch.sh (255 lines)

### Value: **HIGH** — key frame flag fix is critical; H265 header fix prevents buffer overread; pool size config important

---

## 17. izdeveloper/jetson-ffmpeg (3 ahead, 25 behind Keylost) {#izdeveloper}

Issues: disabled | PRs: none | Branches: master, jetpack-6.2.1 (identical)

### Key Changes

- JetPack version-gated nvbuf_utils via `apt show nvidia-jetpack` at cmake time
- Tegra path fix in nvmpi.pc.in

### Value: LOW — creative approach but apt-query at cmake time is fragile

---

## 18. madsciencetist/jetson-ffmpeg (3 ahead, 21 behind Keylost) {#madsciencetist}

Issues: disabled | PRs: 3 merged

### Key Changes

- **stderr redirect** (PR#1): All libnvmpi prints → stderr (correctness for piped video)
- **Decoder thread naming**: pthread name for debugging
- **FFmpeg 6.1 support** (PR#2): Patch + missing `unistd.h` in nvmpi_enc.c
- **nvbuf_utils conditional** (PR#3): Header-based detection (cleanest approach, adopted upstream)
- **Decoder resize option** (add_resize_option2 branch, UNMERGED): `-resize WxH` AVOption using hardware BlockLinear→PitchLinear "free" resize

### Value: MEDIUM — resize option is useful; stderr fix already in our PR#53

---

## 19. w3sip/jetson-ffmpeg (0 ahead, all work on branches) {#w3sip}

Issues: disabled | PRs: 6 (4 merged internally)

### Branches

- **feat/dq** (19 commits): Major decoder refactor + RTSP fix + crash-on-exit fix
- **feat/fix** (1 commit): RTSP extradata fix (isolated)
- **feat/jp5.1** (1 commit): JP 5.1 build fix
- **feat/jp51-2** (15 commits): Subset of feat/dq
- **feat/reproduce** (18 commits): feat/dq + input bitstream save/consume diagnostic
- **release/0.9** (18 commits): Stable subset of feat/dq + FFmpeg 6.1 patches
- **release/0.9.1** (3 commits): Clean cherry-pick of critical fixes

### Key Changes

- **RTSP/extradata fix** (`0d320fe5`): Prime decoder with SPS/PPS — **the original implementation**
- **Decoder crash-on-exit** (`49039f81`): Cleanup queued buffers in destructor
- **Decoder code quality refactor**: Class scoping, private members, mutex automation, error handling
- **CMake NVUtils toggle**: Explicit `WithNVUTILS` option
- **Offline FFmpeg build**: Local source support for air-gapped environments
- **Safer patch creation**: Less fragile `update_patch.sh`

### Value: HIGH — RTSP fix origin; crash-on-exit fix; decoder refactor quality improvements

---

## 20. cybernhl/jetson-ffmpeg (cgutman grandchild) {#cybernhl}

Issues: disabled | PRs: none | Branches: master, low_latency, moonlight_fixes, nvmpi_optional

### moonlight_fixes branch (4 commits by cgutman)

- **Mutex deadlock on EOS** (`58a977d4`): unlock before EOS return check — **CRITICAL**
- **delete vs delete[]** (`ff8fd95a`): Correct array delete in decoder+encoder — **CRITICAL**
- **Uninitialized pointer deletion** (`49d55d10`): nullptr init for all bufptr arrays — **CRITICAL**
- **Hang on close without data** (`00455615`): Adaptive poll timeout for resolution event — **HIGH**

### low_latency branch (moonlight_fixes + 1)

- **condition_variable blocking wait** (`caac9899`): True blocking in `nvmpi_decoder_get_frame` when `AV_CODEC_FLAG_LOW_DELAY` set — **HIGH**

### nvmpi_optional branch (low_latency + 1)

- **Optional NVMPI build** (`b015913a`): `#if CONFIG_NVMPI` guards in FFmpeg

### Value: **CRITICAL** — the 4 safety fixes are must-port; low_latency is must-port

---

## 21. Small Forks + Branch-Only {#small-forks}

### HIGH VALUE

**nhahn** (1 ahead of Keylost): JetPack 6 path auto-detection — `/tegra` vs `/nvidia` conditional in CMake. Simple, correct, important.

**LanderN** (optimize/jetson branch, 6 commits):

- `setMaxPerfMode(1)` + disable DPB
- Camera extradata fix (cites w3sip)
- **glibc 2.34+ pthread_join segfault** (3 commits): Fixes crashes on Ubuntu 22.04+ from uninitialized `dq_thread`, double-free in teardown, vendors patched `NvV4l2ElementPlane.cpp`
- **CRITICAL** for modern distros

**GlassBil** (cuda_buffers branch, 4 commits):

- **CUDA buffer return API** via FFmpeg C API — decoded frames stay on GPU
- **4K I-frame buffer overflow fix**: CHUNK_SIZE 4MB → 10MB + bounds check + configurable `chunk_size`
- Cleanup fixes

**peters** (issue-ffmpeg-6.1 branch, 1 commit):

- Complete FFmpeg 6.1 dev-tree overlay following standard pattern

### MEDIUM VALUE

**xsacha** (4 patch branches): Pixel-format/linesize fixes — yuv420/NV12 switching, linesize corrections
**dthk-cogmatix** (1 ahead): NvBufferSession lifecycle fix in decoder capture loop
**D31m05z** (1 ahead): `pkg_check_modules(libv4l2)` + visibility fix
**danhawker** (1 ahead): extradata-at-init fix for RTSP (cites w3sip)
**matthewj-t6** (1 ahead): Same extradata fix in ffmpeg_dev/ (correct placement)
**therishidesai** (rdesai/jetpack-nixos-build branch): NixOS flake.nix + generalized patcher

### LOW VALUE

**moss-ag/bepro-company/ahtonen**: nvbufsurface pkg-config fix (3 independent discoveries)
**jkms** (deb branch): CPack packaging
**nguyenhunga5**: Ubuntu 22.04 configure docs + vendored NVIDIA SDK (bad practice)
**Hansoluo** (jetpacksupporttable branch): JetPack support table in README
**Extend-Robotics/jetson-ffmpeg-keylost** (packaging branch): CPack module
**fingul**: JP6.2 install script

### TRIVIAL (no code value)

moss-robot, nulijiabei, SynclairVision, AustinTodd-eng, sonnyhe, lyfpeter, timonsku, didebuli, sadhasld (AI docs), Parrot-Developers (atom.mk), JetLinWork, thejasonfisher

---

## 21b. Hidden Branch Forks (found during mirror verification) {#hidden-branch-forks}

These 8 forks appeared as mirrors (0 ahead on master) but had unique work on non-default branches.

### NEW FINDINGS

**bmegli/jetson-ffmpeg** (`new-ffmpeg-api` branch, 1 commit)

- `437f8c55` POC for new FFmpeg nvmpi API implementation
- Implements modern `send_frame`/`receive_packet` encoder API alongside legacy `encode2`
- Adds `ff_nvmpi_send_frame()` + `ff_nvmpi_receive_packet()` as separate entry points
- Adds `encoder_flushing` state for proper EOS drain (old API misses final packets)
- New `nvmpi_dec.c` (171 lines) and `nvmpi_enc.c` (309 lines)
- Refs jocover#131. **Encoder side of this is unique — not found in any other fork.**
- Value: **HIGH** — reference for modern FFmpeg encode API

**tmm1/jetson-ffmpeg** (`fancybits` = `patch-1` branch, 1 commit)

- `428edfec` Fix cflags — CMakeLists.txt variable expansion bug
- Original: literal `$ {CMAKE_C_FLAGS}` (broken expansion)
- Fixed: `"${CMAKE_C_FLAGS} -fPIC"` (properly quoted)
- Value: MEDIUM — real build failure on strict CMake versions

**kazuki0824/jetson-ffmpeg** (`resolve-missing-dep-on-pkgconfig` branch, 4 commits)

- Replaces `find_library(LIB_V4L2 nvv4l2 ...)` with `pkg_check_modules(LIB_V4L2 REQUIRED libv4l2)`
- Introduces `BITBAKE_SYSROOT` variable for Yocto cross-compilation via sysroot injection
- Moves `.pc` install from `share/pkgconfig` to `lib/pkgconfig`
- Adds `Requires: libv4l2` to `nvmpi.pc.in`
- Value: MEDIUM — Yocto cross-compile support is unique; pkg-config approach is cleaner

### KNOWN / DUPLICATE FINDINGS

**cgutman/jetson-ffmpeg** (`moonlight_fixes`, `low_latency`, `nvmpi_optional` branches)

- Same code as cybernhl/jetson-ffmpeg (already analyzed in section 20). cgutman is the original author; cybernhl inherited all branches. 0 ahead on master.
- All 4 safety fixes + low_latency + nvmpi_optional already cataloged.

**uschen-thirdparty/jetson-ffmpeg** (`max_perf_mode` branch, 2 ahead)

- `991f2296` Adds `setMaxPerfMode(1)` in encoder. Known finding (douo, LanderN, bradcagle).

**koenvandesande/jetson-ffmpeg** (`loosen_pixfmt_check` branch, 1 ahead)

- `3c3f608f` YUVJ420P acceptance alongside YUV420P. Known finding (bradcagle `67e7bbcd`).

**gvsyn/jetson-ffmpeg** (`n4.4.1` branch, 2 ahead)

- Patch rebase for FFmpeg n4.4.1 point release. No logic changes. Superseded.

**coupdair/jetson-ffmpeg** (`dev_use` branch, 2 ahead)

- README-only: parallel FFmpeg throughput benchmarks (1-4 instances). Zero code.

---

## 21c. Verified Mirrors (164 forks, no unique work) {#mirror-verification}

All 164 forks below were verified via GitHub API: 0 commits ahead on all branches, no extra branches beyond master, 0 issues, 0 PRs.

**jocover mirrors (135):** BiGZ31, xbkaishui, JAT117, heyfluke, abishekmuthian, MengShuaiLong, beixiaocai, ranjanjyoti152, guoyibiao0007, Strange-mzi, errorcode4319, mcclumpherty, GregSied, feelwa17, ysluodz, GitHangar, Hipoooop, dude84, jo-dean, ENOFFILE, IvanOrfanidi, ericbin1, acavanag, zshaobo, DeserterW, Jerry3062, DanyPoro, ald2004, TatsushiKatayama, rebotnix, sudapure, zxnj, nelsonjt, GeoffreyHub, sunmin7572, roflcoopter, jheo4, fnovoac, LD-1, lauro-cesar, mithrandir42, kevleyski, zodiaq, novaheadend, jbasu2013, pc859107393, anhthoai, arielmol, KevinAnnn, kshvakov, byronwind, ou525, ezverev1980, Xyzhao1999, hlong1981, daryasyr, ranfish, Earl-chen, sdw8855, asdlei99, willkuerliches-archiv, Ojigrande, tosato3, jian488, ringlover, isztldav, Lolopeze, jlerasmus, tuyaliang, pzy2253999075, prophetzopu, GavinDarkglider, Chiyaoching, sy19870112, dlueth, FCLC, chiwenheng, pantoniou, yin-zhang, bstff, binhlv2607, connormcmk, truonggiangmt, PubFork, JiaLiangAl, goel42, GTRedE, PolasekT, jempe, s87315teve, theofficialgman, edmund-troche, voicon, seuwangcheng, RichardPar, finemax, nixx77, weinyzhou, jesusluque, Xavier-John, matcoelhos, mfr78, hgySandy, FREEWING-JP, succo69, TuskAW, GaryMatthews, mstaryi, archsh, leeyunhome, samudra54, agent001, tcll321, hexam-project, HerbertUnterberger, kabobobo, JinhaSong, eagle40, fmogollonr, ccpc, qqq-tech, tolysz, jan-hoelscher-mss, chengman2015, ggnull35, tinyloop, vvhh2002, tankhunter8192, martinhering, efbsolis, zhaoshulin, onearrow, suixiaodan, GengCauWong, moeiscool

**Keylost mirrors (29):** shista2010, yueyihua, qnlbnsl, IlIllllll, Kieaer, jackeyri, f2liu, AutoLifeRobot, jellfits, Dw9, nrobin, berndpfrommer, yxsheron, sminofff, dl1rf, xinsuinizhuan, FunnyWii, Pi-Boss, fancong5201314, jia0511, ACarcione-AthenaAI, kivenyangming, tatsuyai713, pavloshargan, Kevin4ch, hzy5000, Jin2022, zxcv1884, HuiyuanZang

---

## 22. Fork Subtrees {#subtrees}

### douo Subtree

**douo/jetson-ffmpeg** (2 ahead of jocover)

- `d9856840` NvUtils migration (nvbuf_utils → NvBufSurface)
- `e96d1ace` setMaxPerfMode(1) for decoder
- Value: MEDIUM — foundational NvUtils migration + perf

**Tantael/jetson-ffmpeg-fixed** (child of douo, +5 ahead)

- All commits are README edits only. patch-1 branch: 1 unique commit (also README).
- Value: NONE

**Extend-Robotics/jetson-ffmpeg** (child of douo, +8 ahead) — **Stars: 1**

- `57f3f6c3` setMaxPerfMode for encoder AND decoder
- `96803164` H.264 poc-type=2 (display order = decode order; zerolatency-style). Refs: NVIDIA forums, Extend-Robotics/gst-nvvideo4linux2#1
- `0229e5ba` **BUG FIX:** Missing return value in encoder close callback (UB → segfault on L4T 5.1)
- `6d327624` **BUG FIX:** Missing return value in decoder thread dec_capture_loop_fcn (UB → segfault). Refs jocover#127
- `1c100f1c` **POC:** New FFmpeg receive_packet API implementation (705-line patch file: `0001-POC-for-new-FFmpeg-nvmpi-API-implementation.patch`). Refs jocover#131
- `2c1eaceb` Debian packaging: CMake CPack, PACKAGING.md, nvmpi.pc.in, scripts/package.sh
- Value: **HIGH** — two critical UB fixes are must-port; POC new API is reference; Debian packaging reusable

**ReAlexLiu/jetson-ffmpeg** (child of douo, +4 ahead)

- `07e106eb` "兼容jetpack5.0" (JetPack 5.0 compat): massive rewrite of CMakeLists.txt (+90/-65), adds complete ffmpeg_dev/ structure (4.2, 4.4, 6.0 overlays + common codec files), restructures to src/+include/ layout, adds nvUtils2NvBuf.h, NVMPI_bufPool.hpp, NVMPI_frameBuf.hpp
- Value: LOW — reference for repo structure; similar to our current layout

**rai-opensource/jetson-ffmpeg** (child of douo)

- EXACT DUPLICATE of douo/master. 0 unique commits.

### vietnx Subtree

**vietnx/jetson-ffmpeg** (4 ahead, 2 behind jocover; fix-compatible-issue branch)

- `a8edb1c1` Memory leak fix in nvmpi_enc + cleanup in dec/enc
- `fdbb815a` Enable insert_vui to add FPS info to video stream
- `e9568cd1` Fix compatible issue with JetPack 4.4 and 4.5
- `b40817d9` Check for waitpid() failure
- `b0e5aa6e` Make code compatible with JetPack 4.6
- Upstream contribution: PR#79 to jocover (MERGED — linesize/stride crash fix)
- Value: MEDIUM — memory leak fix + insert_vui portworthy; JP4 compat is obsolete

**dlavrantonis/jetson-ffmpeg** (child of vietnx, +5 ahead jocover)

- Carries only 2 of vietnx's 5 code commits (a8edb1c1, fdbb815a). Issue #1 (closed): "pull latest from owner". PR #1 (closed): synced jocover:master. Two README edits.
- Value: NONE — no new code

### aveao Subtree

**aveao/jetson-ffmpeg** (1 ahead of jocover)

- `144f96ed` Port to FFmpeg 4.4 (patch + configuration)
- Value: LOW — superseded by our multi-version support

**jamitupya/jetson-ffmpeg** (child of aveao)

- EXACT DUPLICATE of aveao/master. 0 unique commits.

**EliteScientist/jetson-ffmpeg** (child of aveao)

- EXACT DUPLICATE of aveao/master. 0 unique commits.

**dbussert/jetson-ffmpeg** (child of aveao, +2 ahead)

- `9318e2de` Fix patch for latest FFmpeg 4.4.2 point release
- Value: LOW — superseded

### madsciencetist Grandchildren

**AndBobsYourUncle/jetson-ffmpeg** (child of madsciencetist, +2 ahead, 2 behind — diverged)

- `b8fb1ee3` Remove nvbuf_utils from CMakeLists.txt link deps
- `9c17b09f` CMake install target: don't overwrite, use plain library names for stubbing
- Value: LOW — CMake cleanup

**traversaro/jetson-ffmpeg** (child of madsciencetist, addffmpeg61 branch)

- `c83482ed` Full FFmpeg 6.1 overlay: configure (8153 lines), Makefile (1396 lines), allcodecs.c (1014 lines), ffmpeg6.1_nvmpi.patch (987 lines)
- `9148a0f0` Missing `unistd.h` in nvmpi_enc.c for `usleep()` — 1-line fix propagated to all 4 patch files (4.2, 4.4, 6.0, 6.1)
- Value: LOW — we have 6.1 support; unistd.h fix worth verifying

### izdeveloper Grandchild

**Ledrego/jetson-ffmpeg** (child of izdeveloper)

- Stated "+2 ahead" but actually 0 ahead and 1 BEHIND izdeveloper/master
- Value: NONE — behind parent

### Cross-cutting (F5 subtree summary)

**Duplicates (6/14):** rai-opensource, jamitupya, EliteScientist (exact mirrors); Ledrego (behind parent); dlavrantonis, Tantael (README-only)

**Unique code (8/14):** douo, Extend-Robotics (HIGH), ReAlexLiu, vietnx (MEDIUM), aveao, dbussert, AndBobsYourUncle, traversaro (LOW)

**Upstream issue cross-refs found in fork commits:** jocover#74, #79 (merged), #85, #123, #125, #127 (fixed by Extend-Robotics), #130, #131 (POC by Extend-Robotics), #132

---

## 23. Master Priority List — What to Port {#priority-list}

### CRITICAL (port immediately)

| # | Fix | Source | Commit |
|---|-----|--------|--------|
| 1 | Mutex deadlock on EOS | cgutman/cybernhl | `58a977d4` |
| 2 | delete vs delete[] arrays | cgutman/cybernhl | `ff8fd95a` |
| 3 | Uninitialized pointer deletion on close | cgutman/cybernhl | `49d55d10` |
| 4 | Decoder hang on close without data | cgutman/cybernhl | `00455615` |
| 5 | All frames marked as keyframes (encoder) | spotai | `1324d7d6` |
| 6 | H265 encoder header/IDR scan bounds | spotai | `2b715e81` |
| 7 | Missing return statement in capture callbacks | Extend-Robotics | `0229e5ba`, `6d327624` |
| 8 | glibc 2.34+ pthread_join segfault | LanderN | optimize/jetson branch |
| 9 | 4K I-frame buffer overflow (CHUNK_SIZE) | GlassBil | cuda_buffers branch |

### HIGH (port soon)

| # | Feature/Fix | Source | Reference |
|---|-------------|--------|-----------|
| 10 | condition_variable blocking wait (low-latency decode) | cgutman/cybernhl | `caac9899` |
| 11 | Dynamic bitrate + force IDR APIs | muxable | `1a0c3bdc` |
| 12 | Zero-copy DMA-BUF pipeline (decode fd + encode fd + VIC scale) | YuriiHoliuk | 26 commits |
| 13 | hwcontext_nvmpi (FFmpeg HW device type) | mguzzina | 6 commits (WIP) |
| 14 | P010 10-bit decode | YuriiHoliuk | `4aab1b56` |
| 15 | YUVJ420P→YUV420P remapping | bradcagle | `67e7bbcd` |
| 16 | Configurable frame_pool_size / packet_pool_size | spotai | `cf0560b5` |
| 17 | setMaxPerfMode(1) | bmegli/LanderN | one-liner |
| 18 | Decoder resize option (-resize WxH) | madsciencetist | add_resize_option2 branch |
| 19 | JetPack 6 path auto-detection | nhahn | single commit |

### MEDIUM (evaluate)

| # | Item | Source |
|---|------|--------|
| 20 | Wrong color format ABGR32→XRGB32 | dsleep `018e6e2e` |
| 21 | Encoder hang fix (unique_ptr) | dsleep `17443b0e` |
| 22 | JPEG decode prototype (reference) | bradcagle `cd9bf72e` |
| 23 | Optional NVMPI build (#if CONFIG_NVMPI) | cgutman `b015913a` |
| 24 | dlopen lazy-loading | YuriiHoliuk `b4ede2c9` |
| 25 | Encoder PTS truncation + circular buffer fixes | teplofizik patches.zip |
| 26 | Encoder memory leak cleanup | PR#82 (vietnx) |
| 27 | CUDA buffer return API | GlassBil cuda_buffers branch |
| 28 | Decoder crash-on-exit (buffer cleanup) | w3sip `49039f81` |
| 29 | NV12 pixel format support | xsacha patches |
| 30 | Expanded encoder params (QP/SAR/ROI/lossless) | muxable |
| 31 | AV1 support | requested in Keylost#49 |

### REFERENCE ONLY (different approach than ours)

- Vendor NVIDIA sample classes (dsleep) — we use devcontainer mounts
- Cross-compilation sysroot (muxable) — we target native Jetson builds
- JetPack 4/5 dual-path via lsb_release (dsleep) — our CMake auto-detects
- CPack .deb packaging (muxable, jkms, Extend-Robotics) — evaluate later
- NixOS support (therishidesai) — niche platform
