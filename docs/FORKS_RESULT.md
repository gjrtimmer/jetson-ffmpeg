# Fork Network — Cross-Referenced Analysis & Results

Synthesized analysis of all findings from the fork network sweep.
Cross-references related items, deduplicates overlapping work, and provides
actionable recommendations.

**Generated:** 2026-06-12
**Source data:** [FORKS.md](FORKS.md) (tracking), [FORKS_REPORT.md](FORKS_REPORT.md) (raw findings)

---

## Table of Contents

1. [Deduplicated Fix Clusters](#fix-clusters)
2. [Deduplicated Feature Clusters](#feature-clusters)
3. [Cross-Fork Dependency Graph](#dependency-graph)
4. [Port Priority Matrix](#priority-matrix)
5. [JPEG/MJPEG Specific Analysis](#jpeg-analysis)
6. [Upstream Issue Resolution Map](#upstream-map)
7. [Risk Assessment](#risk-assessment)

---

<a id="fix-clusters"></a>
## 1. Deduplicated Fix Clusters

Many forks independently discovered or ported the same fixes. This section
groups related fixes to identify the best source for each.

### FC-1: Decoder Memory Safety (cgutman — 4 fixes)

**Root cause:** Original decoder has multiple memory safety issues in lifecycle management.

| Fix | Commit | Impact |
|-----|--------|--------|
| Mutex deadlock on EOS | `58a977d4` | Decoder hangs at stream end |
| `delete` vs `delete[]` | `ff8fd95a` | Heap corruption (decoder + encoder) |
| Uninitialized pointer deletion | `49d55d10` | Crash if < MAX_BUFFERS used |
| Hang on close without data | `00455615` | Indefinite hang if no frames sent |

**Sources:** cgutman (original author) → cybernhl (inherits). No other fork has these.
**Best source:** cybernhl/jetson-ffmpeg `moonlight_fixes` branch — all 4 in sequence.
**Cross-refs:** jocover#10, #104 (thread lifecycle); Moonlight game streaming use case.
**Status in our repo:** NONE ported. All 4 are CRITICAL.

### FC-2: Missing Return Statement UB (2 locations)

**Root cause:** V4L2 capture callback functions declared `void*` but have no `return` statement. Newer GCC on L4T 5.1+ treats this as UB → segfault.

| Location | Commit | Author |
|----------|--------|--------|
| Encoder: `enc_capture_loop_fcn` | `0229e5ba` | Extend-Robotics |
| Decoder: `dec_capture_loop_fcn` | `6d327624` | Extend-Robotics |

**Sources:** Extend-Robotics only fork with explicit fix.
**Cross-refs:** jocover#127 (decoder segfault reports), jocover#98.
**Status in our repo:** NEEDS VERIFICATION — may already be addressed differently in Keylost lineage.

### FC-3: Key Frame Flag Bug (encoder)

**Root cause:** `nvmpi_enc.c` line ~92 does `pkt->flags |= 0x0001` (AV_PKT_FLAG_KEY) on EVERY packet. Result: every frame marked as keyframe. Breaks seeking, GOP structure, rate control, HLS/DASH segmentation.

| Fix | Commit | Author |
|-----|--------|--------|
| Use `enc_metadata.KeyFrame` from V4L2 | `1324d7d6` | spotai |

**Sources:** spotai only. Filed as Keylost#26 (UNFIXED upstream).
**Cross-refs:** Keylost#26.
**Status in our repo:** NOT FIXED. CRITICAL — silent corruption of every encoded stream.

### FC-4: H.265 Encoder Header / IDR Scan

**Root cause:** GLOBAL_HEADER scan for SPS/PPS NALs was (a) not applied to H.265, (b) had unbounded scan loop that could read past buffer end.

| Fix | Commit | Author |
|-----|--------|--------|
| Extended to H.265 + bounded scan | `2b715e81` | spotai |

**Sources:** spotai only.
**Status in our repo:** NOT FIXED. CRITICAL — buffer overread potential.

### FC-5: glibc 2.34+ pthread Segfaults

**Root cause:** `NvV4l2ElementPlane` has uninitialized `pthread_t dq_thread`. On glibc 2.34+ (Ubuntu 22.04+), `pthread_join()` on uninitialized `pthread_t` segfaults instead of returning an error.

| Fix | Commits | Author |
|-----|---------|--------|
| Init dq_thread, fix teardown, vendor patched NvV4l2ElementPlane.cpp | 3 commits | LanderN |

**Sources:** LanderN only (optimize/jetson branch).
**Cross-refs:** Keylost#21, PR#48 (Ubuntu 22.04 compilation issues).
**Note:** This patches NVIDIA's sample code, not our code. Need to vendor or upstream the fix.
**Status in our repo:** NOT FIXED. CRITICAL for Ubuntu 22.04+.

### FC-6: 4K I-Frame Buffer Overflow

**Root cause:** `CHUNK_SIZE` (input buffer for encoded data) hardcoded at 4MB. 4K I-frames can exceed this → truncation or crash.

| Fix | Source | Author |
|-----|--------|--------|
| CHUNK_SIZE 4→10MB + bounds check + configurable `chunk_size` | cuda_buffers branch | GlassBil |

**Sources:** GlassBil only.
**Status in our repo:** NOT FIXED. HIGH — affects 4K encoding.

### FC-7: RTSP / Out-of-Band SPS/PPS Decoder Hang

**Root cause:** Decoder hangs when SPS/PPS is only in SDP (not in-band). Decoder needs extradata fed before first frame.

| Fix | Source | Approach |
|-----|--------|----------|
| Prime decoder with extradata at init | w3sip `0d320fe5` | Original implementation |
| Same fix, different approach | hsuanguo PR#3 | Co-authored with w3sip |
| Same fix | gjrtimmer `08e32c15` | Our implementation |
| extradata-at-init | danhawker, matthewj-t6 | Same concept |
| Partial (camera-specific) | LanderN | Cites w3sip |

**This is the most-duplicated fix in the entire network.** 5+ independent implementations.
**Status in our repo:** FIXED (gjrtimmer@08e32c15). Not merged upstream to Keylost.

### FC-8: Decoder Crash on Exit

**Root cause:** Queued DMA buffers not cleaned up in destructor → crash on rapid open/close or error paths.

| Fix | Source | Author |
|-----|--------|--------|
| Cleanup queued buffers in destructor | `49039f81` | w3sip |
| Related: adaptive poll timeout for resolution event | `00455615` | cgutman |

**Two different aspects** of the same lifecycle problem. Both should be ported.
**Status in our repo:** NOT FIXED.

### FC-9: Child Process Decoder Segfault

**Root cause:** V4L2 fd inheritance after fork() breaks decoder. NvBufferSession create/destroy around transform fixes it.

| Fix | Source | Author |
|-----|--------|--------|
| NvBufferSession lifecycle | xia-chu `fd0900d7` (via Keylost fork_fix) | Keylost/xia-chu |
| nvjpeg removal (related) | hsuanguo PR#2 | Removes unused dep |

**Status in our repo:** VERIFY — may already be in Keylost lineage we inherited.

---

<a id="feature-clusters"></a>
## 2. Deduplicated Feature Clusters

### FT-1: Zero-Copy / GPU Pipeline

Multiple forks approach GPU-resident decode/encode from different angles:

| Approach | Author | Maturity | Files |
|----------|--------|----------|-------|
| **Full DMA-BUF pipeline** (get_frame_fd/put_frame_fd + DRM_PRIME) | YuriiHoliuk | Most complete (26 commits) | nvmpi_dec.cpp, nvmpi_enc.cpp, nvmpi.h, nvmpi_surface.cpp |
| **VIC hardware scale filter** (vf_scale_vic.c) | YuriiHoliuk | Working | +468 lines new file |
| **hwcontext_nvmpi** (FFmpeg HW device type) | mguzzina | WIP (6 commits) | hwcontext_nvmpi.c/h |
| **CUDA buffer return** (frames stay on GPU via FFmpeg API) | GlassBil | Working | cuda_buffers branch |
| **dlopen lazy-loading** (prevent EGL crash) | YuriiHoliuk | Working | dynlink_nvmpi.h |
| **dlopen** (independent impl) | wangxiaoshuang | Working | different approach |

**These are complementary, not competing:**
- YuriiHoliuk's DMA-BUF pipeline handles the V4L2↔FFmpeg data path
- mguzzina's hwcontext_nvmpi provides the FFmpeg framework integration
- GlassBil's CUDA buffer return provides the CUDA interop
- dlopen prevents startup crashes when libnvmpi is missing

**Recommended port order:** dlopen → DMA-BUF pipeline → hwcontext → VIC filter → CUDA

### FT-2: Encoder Control APIs

| Feature | Author | API |
|---------|--------|-----|
| Dynamic bitrate | muxable | `nvmpi_encoder_set_bitrate(ctx, bitrate)` |
| Force IDR | muxable | `nvmpi_encoder_force_idr(ctx)` |
| Per-frame QP, SAR, bit_depth, chroma_format_idc | muxable | Extended nvPacket struct |
| Lossless encode | muxable | `enableLossless` (also requested in Keylost#45) |
| ROI encode | muxable | Region of interest |
| CABAC toggle, VUI insert, AUD insert | muxable | Additional encoder params |

**Single source:** muxable. Most comprehensive encoder param expansion.
**Cross-refs:** Keylost#45 (lossless), Keylost#50 (setLevel), jocover#50,#52,#7 (quality control).

### FT-3: Performance Optimization

| Optimization | Author(s) | Impact |
|-------------|-----------|--------|
| `setMaxPerfMode(1)` | douo, Extend-Robotics, LanderN, bradcagle | 4K HEVC: 28→64fps; 720p H264: 46→290fps |
| Disable DPB | LanderN | Additional decode perf |
| condition_variable blocking wait | cgutman | Eliminates polling; true zero-latency |
| poc-type=2 (display=decode order) | Extend-Robotics | Reduces latency for H.264 |

**setMaxPerfMode is a one-liner** that every perf-focused fork independently discovers. We should port it immediately.

### FT-4: Pixel Format / Color Space

| Fix/Feature | Author | Commit |
|-------------|--------|--------|
| YUVJ420P→YUV420P remap | bradcagle | `67e7bbcd` |
| NV12 format support | xsacha | 4 patches |
| P010 10-bit decode | YuriiHoliuk | `4aab1b56` |
| ABGR32→XRGB32 color fix | dsleep | `018e6e2e` |
| insert_vui for FPS | vietnx | `fdbb815a` |

**Related upstream issues:** jocover#20, #60, #66, #68, #73, #83, #116, #75, #80, #11, #133.
**Pixel format is one of the most-reported issue categories.** NV12 + YUVJ420P + 10-bit together would resolve most complaints.

### FT-5: Pool Size Configuration

| Feature | Author | Why |
|---------|--------|-----|
| `frame_pool_size` option | spotai | Prevents EAGAIN on frame pool exhaustion |
| `packet_pool_size` option | spotai | Prevents EAGAIN on packet pool exhaustion |
| `chunk_size` option | GlassBil | Configurable input buffer for 4K+ |

**Cross-refs:** Keylost#38 (EAGAIN workaround: `-packet_pool_size:v 32`).
**These should be ported together** as they address the same "hardcoded limits" problem.

### FT-6: Codec Flush / Lifecycle

| Issue | Status | Related Forks |
|-------|--------|---------------|
| Codec flush not implemented | Keylost#42 UNFIXED | No fork has a fix |
| Encoder thread race (packet_pools) | jocover#130 UNFIXED | bmegli identified; no fix |
| Non-monotonic DTS | jocover#76, #110 | teplofizik patches (partial) |

**Gap:** No fork in the entire network has implemented proper codec flush. This is an open problem.

### FT-7: Modern FFmpeg Encode API (send_frame / receive_packet)

| Approach | Author | Maturity |
|----------|--------|----------|
| **send_frame/receive_packet encoder** (coexists with legacy encode2) | bmegli | POC (1 commit, new-ffmpeg-api branch) |
| **POC patch** (receive_packet only, 705-line patch file) | Extend-Robotics | Reference only (`1c100f1c`) |

**Root cause:** Legacy `encode2` callback blocks synchronously — 2-5 frame latency, can miss final packets at EOS.
**bmegli's implementation** adds `encoder_flushing` state for proper EOS drain that the old API lacks.
**Cross-refs:** jocover#131 (encoder latency), jocover#7, Keylost#42 (flush).
**Note:** This is closely related to FT-6 (codec flush) — the new API is the prerequisite for proper flush.

### FT-8: Build System / Cross-Compilation

| Feature | Author | Unique? |
|---------|--------|---------|
| CMake cflags quoting fix | tmm1 `428edfec` | YES — real bug, breaks strict CMake |
| Yocto/BITBAKE_SYSROOT cross-compile | kazuki0824 (4 commits) | YES — embedded Linux use case |
| Cross-compile via TARGET_ROOTFS | muxable | Different approach (Debian sysroot) |
| CPack .deb packaging | muxable, jkms, Extend-Robotics | Multiple implementations |
| pkg-config for libv4l2 | kazuki0824, D31m05z | Two independent approaches |

**tmm1's cflags fix** is a real bug that affects all builds on strict CMake — should port.

---

<a id="dependency-graph"></a>
## 3. Cross-Fork Dependency Graph

```
jocover/jetson-ffmpeg (abandoned ~2022)
├── Keylost/jetson-ffmpeg (active maintainer, 74 ahead)
│   ├── YuriiHoliuk (+26): DMA-BUF, VIC, P010, dlopen
│   ├── mguzzina (+6): hwcontext_nvmpi
│   ├── wangxiaoshuang (+9): dlopen, decoder rewrite
│   ├── hsuanguo (+8): RTSP fix, JP5.1.2, FFmpeg 6.1
│   ├── fingul (+7): JP6.2 install
│   ├── v-faeez-kadiri (+5) → spotai (+20 on branch): KEY FRAME FIX, H265, FFmpeg 8.0
│   ├── izdeveloper (+3) → Ledrego (behind)
│   ├── madsciencetist (+3): resize, stderr → AndBobsYourUncle (cmake) → traversaro (6.1)
│   ├── w3sip (branches only): RTSP fix origin, crash-on-exit, refactor
│   ├── nhahn (+1): JP6 path detect
│   ├── LanderN (branch): glibc fix, setMaxPerfMode
│   ├── GlassBil (branch): CUDA buffers, 4K overflow fix
│   ├── peters (branch): FFmpeg 6.1
│   └── [mirrors: 30+ forks with 0 unique work]
│
├── cgutman (Moonlight) → cybernhl: 4 safety fixes + low-latency
├── dsleep (+32): vendor NVIDIA classes, color fix, encoder fix, JP4/5
├── muxable (+28): dynamic bitrate, force IDR, expanded params
├── xia-chu (+16): child process fix
├── vietnx (+4): leak fix, insert_vui, linesize fix (PR#79 merged)
├── bradcagle (+7): JPEG prototype, YUVJ fix, NvUtils migration ref
├── douo (+2) → Extend-Robotics (+8): UB fixes, POC new API
│            → ReAlexLiu (+4): JP5 compat
├── bmegli (branch): new send_frame/receive_packet encoder API POC
├── xsacha (branches): pixel format fixes
├── dthk-cogmatix (+1): NvBufferSession lifecycle
├── tmm1 (branch): CMake cflags quoting fix
├── kazuki0824 (branch): Yocto cross-compile + pkg-config
├── koenvandesande (branch): YUVJ420P acceptance (= bradcagle)
├── uschen-thirdparty (branch): setMaxPerfMode (= douo/LanderN)
└── [164 verified mirrors with 0 unique work on any branch]
```

**Total coverage:** 237 forks enumerated → 70 with unique work analyzed → 164 verified mirrors → 3 trivial (README/docs only)

---

<a id="priority-matrix"></a>
## 4. Port Priority Matrix

Scoring: Impact (1-5) x Confidence (1-5) x Effort (inverse, 5=easy, 1=hard)

| # | Item | Impact | Confidence | Effort | Score | Category |
|---|------|--------|------------|--------|-------|----------|
| 1 | Key frame flag fix | 5 | 5 | 5 | 125 | FC-3 |
| 2 | cgutman 4 safety fixes | 5 | 5 | 4 | 100 | FC-1 |
| 3 | setMaxPerfMode(1) | 4 | 5 | 5 | 100 | FT-3 |
| 4 | Missing return UB fixes | 5 | 4 | 5 | 100 | FC-2 |
| 5 | H265 header/IDR scan fix | 5 | 4 | 5 | 100 | FC-4 |
| 6 | glibc 2.34+ pthread fix | 5 | 4 | 3 | 60 | FC-5 |
| 7 | 4K I-frame overflow fix | 4 | 4 | 4 | 64 | FC-6 |
| 8 | Pool size configuration | 3 | 5 | 4 | 60 | FT-5 |
| 9 | condition_variable wait | 4 | 4 | 3 | 48 | FT-3 |
| 10 | YUVJ420P remap | 3 | 5 | 5 | 75 | FT-4 |
| 11 | Dynamic bitrate + IDR | 4 | 4 | 3 | 48 | FT-2 |
| 12 | Crash-on-exit fix | 3 | 4 | 4 | 48 | FC-8 |
| 13 | JetPack 6 path detect | 3 | 5 | 5 | 75 | — |
| 14 | CMake cflags quoting fix | 2 | 5 | 5 | 50 | FT-8 |
| 15 | New encode API (send_frame/receive_packet) | 4 | 3 | 2 | 24 | FT-7 |
| 16 | DMA-BUF pipeline | 5 | 3 | 1 | 15 | FT-1 |
| 17 | hwcontext_nvmpi | 5 | 2 | 1 | 10 | FT-1 |
| 18 | Yocto cross-compile | 2 | 4 | 3 | 24 | FT-8 |

### Recommended Port Waves

**Wave 1 — Safety fixes (1 PR, should be fast):**
Items 1-5, 7, 14. All are isolated fixes with clear commits to cherry-pick. Includes CMake cflags fix.

**Wave 2 — Performance + usability (1-2 PRs):**
Items 3, 8, 9, 10, 12, 13. setMaxPerfMode is one line; pool sizes are config additions; YUVJ remap is small.

**Wave 3 — Encoder features (1 PR):**
Items 11, plus expanded encoder params from muxable.

**Wave 4 — glibc fix (1 PR, needs care):**
Item 6. Requires vendoring patched NVIDIA code. May need upstream engagement.

**Wave 5 — Modern encode API (1 PR):**
Item 15. bmegli's send_frame/receive_packet POC as starting point. Prerequisite for proper codec flush.

**Wave 6 — GPU pipeline (multi-PR, major feature):**
Items 16, 17. DMA-BUF + hwcontext. Months of work but highest long-term value.

---

<a id="jpeg-analysis"></a>
## 5. JPEG/MJPEG Specific Analysis

### Upstream Demand Signal

| Ref | What |
|-----|------|
| Keylost#37 | "High cpu usage when encoding" — JPG sequence → h264_nvmpi. Software JPEG decode burns >1 CPU core. jtop: NVENC active, **NVJPG OFF** |
| Keylost#40 | "Accelerated JPEG decoding and encoding" — direct feature request. Keylost: "I'll consider it when I have time" (2025-08) |
| Keylost#45 | Lossless-HEVC bug, but workflow is `-f image2 -i %d.jpg` with 4K JPEGs — third JPEG-input user |
| Keylost#47 | "support direct access to nvbuffer" — zero-copy follow-up that pairs with JPEG |
| Keylost comment on #37 | Confirms root cause: "load could be reduced by using NvJpegDecoder … passing the hardware buffers … directly to the encoder without copying" |

### Prior Art in Fork Network

| Source | What | Status |
|--------|------|--------|
| bradcagle `cd9bf72e` | NvJPEGDecoder C API prototype: `nvmpi_create_jpeg_decoder`, `nvmpi_decode_jpeg`, `nvmpi_close_jpeg_decoder` | Incomplete — no FFmpeg codec wrapper, no encoder |
| Keylost#37, #40 | Feature requests for JPEG HW decode/encode | UNFIXED |
| hsuanguo PR#2 | Removed nvjpeg (unused dep) | Merged — confirmed JPEG was never functional |
| Keylost PR#9 | Added nvjpeg stubs | Merged — stubs only, no implementation |
| NVIDIA forum /t/189290 | nvjpeg causes segfault in child processes | Root cause documented |

**No fork in the entire 237-fork network has a working JPEG implementation.** (Confirmed by exhaustive sweep.)

### Hardware Support Matrix

| Module | NVJPG decode | NVJPG encode |
|--------|-------------|-------------|
| Nano (T210), TX2, Xavier NX, AGX Xavier | 600 MP/s | Yes |
| AGX Orin, Orin NX (T234) | 2x engines (2x600 MP/s) | Yes |
| **Orin Nano** | Decode-only (499.2 MHz max) | **NO** (NVIDIA confirmed) |

- Engine: NVIDIA Tegra-accelerated libjpeg-8b fork (`libnvjpeg.so`), NOT CUDA nvJPEG
- Limits: baseline JPEG only (no progressive), 8 bpc, max 16384x16384
- Decode: 420/422H/V/444/400; Encode: YUV420/NV12 input only

### Proposed Design

**Backend:** `NvJPEGDecoder::decodeToFd` / `NvJPEGEncoder::encodeFromFd` (documented MMAPI classes, present in devcontainer's mounted MMAPI). Identical signatures JP4→JP6.

**Shape:**
- `include/nvmpi.h`: add `NV_VIDEO_CodingMJPEG` to `nvCodingType`
- `src/nvmpi_jpegdec.cpp` (new): synchronous decode — `put_packet` → `decodeToFd` → VIC transform → frame pool → `get_frame` copy-out. No capture thread, no resolution events.
- `src/nvmpi_jpegenc.cpp` (new, phase 2): copy raw YUV420 into DMA buffer → `encodeFromFd(quality)` → nvPacket. Map FFmpeg `-q:v` → libjpeg quality 1-100.
- FFmpeg layer: `NVMPI_DEC(mjpeg, AV_CODEC_ID_MJPEG, NULL)` + encoder registration
- Output `yuv420p` + `color_range = AVCOL_RANGE_JPEG` (not deprecated yuvj420p)
- `scripts/ffpatch.sh`: anchor `mjpeg_cuvid_decoder_deps="cuvid"` (present in all 7 FFmpeg versions)
- CMake: `libnvjpeg` already linked; add `NvJpegDecoder.cpp`/`NvJpegEncoder.cpp` to sources

**Phasing:**
1. v1: `mjpeg_nvmpi` decoder only — resolves #37's CPU on ALL modules incl. Orin Nano
2. v2: `mjpeg_nvmpi` encoder — everywhere except Orin Nano
3. Follow-up: zero-copy decode→encode (DMA fd passthrough; Keylost#47 territory)

### Risks

- JP5.1.x `decodeToFd` stale-frame regression (NVIDIA-acknowledged, forum /t/264120)
- Progressive JPEG: HW unsupported — detect SOF2 and fail with clear error
- 422/444 JPEGs: decode OK, VIC converts to YUV420 → chroma loss in v1
- Orin Nano NVJPG decode clock low by default pre-JP5.1.2 (115 MHz)
- nvjpeg causes fork() segfaults (FC-9) — must handle in implementation

### Relation to Other Findings

- **FC-9 (child process segfault)**: nvjpeg was specifically identified as causing fork() segfaults
- **FT-1 (zero-copy pipeline)**: JPEG decode should support DMA-BUF output for VIC scaling
- **FT-4 (pixel format)**: JPEG needs YUVJ420P/YUV422P/YUV444P support
- **FT-7 (new encode API)**: JPEG encoder benefits from send_frame/receive_packet pattern
- **bradcagle's prototype**: Starting point for C API, needs FFmpeg wrapper + encoder

---

<a id="upstream-map"></a>
## 6. Upstream Issue Resolution Map

How fork findings map to upstream issues:

| Upstream Issue | Fork Resolution | Best Source |
|---------------|----------------|-------------|
| jocover#127 (decoder segfault) | Extend-Robotics `6d327624` | Extend-Robotics |
| jocover#131 (encoder latency) | Extend-Robotics POC `1c100f1c` | Extend-Robotics (reference) |
| jocover#130 (encoder thread race) | **STILL UNFIXED** | — |
| jocover#120 (setMaxPerfMode) | douo, LanderN, bradcagle | Any (one-liner) |
| jocover#41,#48,#100 (PTS truncation) | teplofizik patches.zip | teplofizik |
| jocover#20,#60+ (YUVJ420P) | bradcagle `67e7bbcd` | bradcagle |
| jocover#64,#80 (linesize crash) | vietnx PR#79 (merged) | Already upstream |
| jocover#11,#73 (NV12 support) | xsacha 4 patches | xsacha |
| jocover#75,#80 (10-bit) | YuriiHoliuk P010 | YuriiHoliuk |
| jocover#62,#67+ (CUDA/zero-copy) | YuriiHoliuk, GlassBil, mguzzina | Combined approach |
| jocover#117,#118 (DMA encode) | YuriiHoliuk put_frame_fd | YuriiHoliuk |
| Keylost#26 (keyframe flag) | spotai `1324d7d6` | spotai |
| Keylost#42 (codec flush) | **STILL UNFIXED** | — |
| Keylost#37,#40 (JPEG) | bradcagle prototype (incomplete) | bradcagle (reference) |
| Keylost#45 (lossless H265) | muxable `enableLossless` param | muxable |
| Keylost#49 (AV1) | **STILL UNFIXED** | Hardware capable on AGX Orin |
| Keylost#50 (H265 setLevel) | **STILL UNFIXED** | — |
| Keylost#31,#47 (hwcontext) | mguzzina WIP | mguzzina |

**Permanently unfixed across entire network (no fork has a solution):**
- Encoder thread race condition (jocover#130)
- Codec flush (Keylost#42)
- AV1 support (Keylost#49)
- H265 setLevel (Keylost#50)

---

<a id="risk-assessment"></a>
## 7. Risk Assessment

### High-Confidence Ports (well-tested, clear commits)

- **cgutman safety fixes**: Battle-tested in Moonlight game streaming (latency-sensitive)
- **spotai key frame fix**: Isolated, obvious bug with clear before/after behavior
- **setMaxPerfMode**: One-liner, no risk, massive perf gain
- **YUVJ420P remap**: Simple conditional, well-understood
- **Pool size config**: Additive (new AVOptions), no behavioral change to defaults

### Medium-Risk Ports (need adaptation)

- **glibc pthread fix**: Vendors patched NVIDIA code — needs maintenance strategy
- **4K overflow fix**: Changes buffer size constant — verify no regressions at lower resolutions
- **condition_variable wait**: Changes decoder threading model — needs careful testing
- **Extend-Robotics UB fixes**: Simple `return NULL;` additions but verify our code structure

### High-Risk Ports (major refactoring)

- **DMA-BUF pipeline**: 26 commits touching decoder, encoder, and FFmpeg codec layer
- **hwcontext_nvmpi**: WIP; modifies FFmpeg's hardware device framework
- **Dynamic bitrate**: Adds new API surface — must design for stability
- **Decoder resize**: Unmerged even in madsciencetist's own repo; incomplete

### Items Requiring Our Own Implementation (no complete fork solution)

- JPEG/MJPEG hardware decode+encode (bradcagle prototype is incomplete)
- Codec flush (no fork has solved this)
- AV1 decode/encode (hardware-capable, zero software work anywhere)
- Full hwcontext integration (mguzzina's WIP is starting point only)
