# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

**jetson-ffmpeg** enables hardware-accelerated H.264/HEVC/MPEG2/MPEG4/VP8/VP9 video encode/decode on NVIDIA Jetson via FFmpeg. This repo is the **FFmpeg integration only** (`ffmpeg/dev/`, `ffmpeg/patches/`): codec source files (`AVCodec`/`FFCodec` wrappers) that call libnvmpi through `dlopen` and get *patched into* a vanilla FFmpeg tree, then compiled as part of FFmpeg.

**libnvmpi lives in its own repo, `timmertech/jetson-nvmpi`** (GitHub mirror `gjrtimmer/jetson-nvmpi`; issues on GitHub, MRs on GitLab). It is a standalone C-API shared library wrapping NVIDIA's V4L2/NvBuffer multimedia API. This repo consumes a **released** jetson-nvmpi package, pinned once as `NVMPI_VERSION` in `.gitlab-ci.yml` (generic package registry tarballs per JetPack; debs `libnvmpi1`/`libnvmpi-dev` for installs). libnvmpi code, its unit/API tests, stubs, mock, CMake build and driver-level fixes go to jetson-nvmpi, not here. The nvmpi mock is built from `NVMPI_MOCK_REF` (default tag `v${NVMPI_VERSION}`; may be a jetson-nvmpi commit SHA for unreleased mock fixes). To adopt a new libnvmpi: use the `/bump-nvmpi` skill (bumps the pin and syncs docs, examples and wiki).

FFmpeg needs neither libnvmpi headers nor libraries at build time (`-ldl` only); at runtime it dlopens `libnvmpi.so.1`, then `libnvmpi.so`; the integration layer is delivered as patches users apply to their own FFmpeg checkout.

## Repository layout

- `ffmpeg/dev/` — FFmpeg patch *development* tree: shared codec sources (`common/`), per-version overlays (`4.2/`, `4.4/`, `6.0/`), and the patch-generation scripts (`update_patch.sh`, `copy_files.sh`, `try_build.sh`).
- `ffmpeg/patches/` — generated `ffmpeg<ver>_nvmpi.patch` files (artifacts; never hand-edit).
- `scripts/` — operator scripts: `build.sh` (patch + build FFmpeg; `--nvmpi-prefix` for a libnvmpi outside the system paths), `fetch-nvmpi.sh` (download a released jetson-nvmpi package into a prefix) and `ffpatch.sh` (runtime FFmpeg patcher). All scripts resolve the repo root from their own location, so they run from any working directory.
- `test/` — per-feature hardware suites (`hw-*.sh`) run by `hw-all.sh` (auto-discovery; see `test/README.md`), `gen-samples.sh` (shared sample generators), and `smoke-all.sh` (full cross-version build + hw-all). FFmpeg sources are fetched by `scripts/clone-ffmpeg.sh`.
- Prose documentation lives in the **[project wiki](https://github.com/gjrtimmer/jetson-ffmpeg/wiki)** (`jetson-ffmpeg.wiki.git`), not in-repo — the `docs/` folder has been retired. Script/alias reference: [Scripts and Commands](https://github.com/gjrtimmer/jetson-ffmpeg/wiki/Scripts-and-Commands).

Supported FFmpeg versions: 6.0, 6.1, 7.0, 7.1, 8.0, 8.1, 9.0 (libavcodec 60→63). 4.2 and 4.4 are retired since v3.0.0 (last release: v2.10.0). To add one, use the `add-ffmpeg-version` skill.

## Build & test commands

In the dev container these are also exposed as aliases (`build`, `ffpatch`,
`update-patch`, `try-build`, `hw-all`) — see [Scripts and Commands](https://github.com/gjrtimmer/jetson-ffmpeg/wiki/Scripts-and-Commands). There is
deliberately no `test` alias (it would shadow the shell builtin).

```bash
# libnvmpi comes from a jetson-nvmpi release: libnvmpi1 deb, or a tarball prefix
./scripts/fetch-nvmpi.sh --version 4.6.5 --dest ~/nvmpi   # public GitHub release, no token; --source gitlab needs GITLAB_TOKEN
# Patch + build a full FFmpeg <ver> with nvmpi (alias: build)
./scripts/build.sh --ffmpeg 7.1 [--nvmpi-prefix ~/nvmpi] [--install]

# Patch a vanilla FFmpeg tree (auto-detects version, idempotent)
./scripts/ffpatch.sh /path/to/ffmpeg
cd /path/to/ffmpeg && ./configure --enable-nvmpi && make

# Regenerate all patch files after editing the integration layer (clones FFmpeg, patches, diffs)
./ffmpeg/dev/update_patch.sh

# Build-validate every supported FFmpeg version
./ffmpeg/dev/try_build.sh

# All hardware test suites against an installed ffmpeg (requires real Jetson; no software fallback)
JETSON_VARIANT=orin-nano ./test/hw-all.sh
HW_SUITES="decoder-chunk encoder-gop" ./test/hw-all.sh   # subset of suites

# Full cross-version smoke test: patch+build+hw-test EVERY version (libnvmpi installed or --nvmpi-prefix)
./test/smoke-all.sh                          # all 7 versions
./test/smoke-all.sh -v "4.2 6.0 8.0"         # subset
```

There is no unit-test suite. Verification is layered: per-feature hardware suites (`test/hw-*.sh`, run by `test/hw-all.sh`; documented in `test/README.md`), the full cross-version harness (`test/smoke-all.sh`), and CI. New features/fixes ship together with the suite that guards them.

### Fast iteration dev process

For iterative development/debugging, use a tight local loop instead of
waiting for the full CI pipeline (~2h10m). The devcontainer runs on JP6
(R36.4.7, Orin Nano) — local tests validate JP6 behavior directly.

```bash
# 1. Install libnvmpi once (libnvmpi1 deb or fetch-nvmpi.sh prefix)

# 2. Build ONE FFmpeg version (~2 min)
./scripts/build.sh --ffmpeg 7.1 [--nvmpi-prefix ~/nvmpi]

# 3. Test ONE suite (~1 min)
export PATH="/home/vscode/ffmpeg-build/ffmpeg7.1:$PATH"
HW_SUITES="filter-scale-vic" JETSON_VARIANT=orin-nano ./test/hw-all.sh

# 4. Fix → repeat steps 2-3 until green (~5 min per cycle)
# 5. Push to CI pipeline for full version matrix validation
```

**Version gate awareness:** When fixing version-specific issues, always
check if the fix needs `#if LIBAVCODEC_VERSION_MAJOR` or
`#ifdef WITH_NVUTILS` guards. Build at least one old (6.0) and one new
(8.0+) version locally before pushing to confirm no regressions.

**CI is for final validation only.** Use the local loop for development.
Push to pipeline when the local test passes. The pipeline validates all
7 FFmpeg versions × 2 JetPack versions — don't iterate on pipeline
failures when you can reproduce locally.

**Validate code changes before considering them done.** Two valid paths:
(a) local `./test/smoke-all.sh` run (7/7 green) before pushing, or
(b) push to an MR branch and let the CI pipeline run the full matrix — then
move to other work while monitoring. Path (b) is preferred when parallelizing
across sessions. Docs-only changes are exempt and may push with `-o ci.skip`.
**Release commits are exempt from the smoke-all gate.** When creating a
release (CHANGELOG.md update + tag), the code has already been validated by the
MR pipeline. Push the release commit and tag directly to main without a local
smoke-all run.

**Push release commits with `-o ci.skip`.** The release commit (CHANGELOG.md
update) only changes documentation — it does not need its own main pipeline.
The tag push triggers the release pipeline, which is the one that matters.
Without `-o ci.skip` on the main push, GitLab runs a redundant main pipeline
alongside the tag pipeline, wasting runner time and competing for Jetson hw.
Sequence: `git push origin main -o ci.skip` then `git push origin v<ver>`.

**Release names must be `vX.Y.Z`.** Both GitLab and GitHub releases use the
tag name as the display title — never `jetson-ffmpeg X.Y.Z` or any other
format. The release script (`scripts/release.sh`) passes `--name "v${VERSION}"`
to `release-cli create`. The tag pipeline's `release:github` job
(`scripts/release-github.sh`) creates (or updates) the matching GitHub release
with the same name, notes and assets; do not create it by hand. The push-mirror
only syncs the tag.

**Never put `[ci skip]` or `[skip ci]` in commit messages.** These tokens
poison every pipeline that includes the commit — branch pipelines AND MR
pipelines. Use `-o ci.skip` on `git push` instead; that flag is scoped to the
push event only and does not affect MR pipelines.

**Skip the branch pipeline when opening an MR immediately.** Pushing a
branch triggers a branch pipeline; creating an MR triggers a second
(merge-request) pipeline. When the intent is to open an MR right after the
push, use `-o ci.skip` on the first push to suppress the redundant branch
pipeline — the MR pipeline is the one that matters:
`git push -u origin <branch> -o ci.skip` → `glab mr create …`.

**Always create MRs with auto-merge** (`glab mr create` then
`glab mr merge <nr> --auto-merge`). Auto-merge waits for the pipeline to pass
before merging — never force-merge or merge manually while a pipeline is
running. The GitLab project requires pipelines to succeed before merge
(`only_allow_merge_if_pipeline_succeeds`). **Wait for the pipeline to be
running before setting auto-merge** — `glab mr merge --auto-merge` returns
HTTP 405 both when no pipeline exists AND when the pipeline exists but is still
`pending` (no runner assigned yet). Poll
`glab api projects/.../merge_requests/<iid>` until
`head_pipeline.status` is `running` (not just non-null) before enabling
auto-merge. With ff merges any push to `main` (docs-only included) makes open
MRs `need_rebase`, and `--auto-merge` then returns 405 too: push docs to `main`
only while no MR is mid-pipeline, or fold them into the next MR.

CI fetches the pinned jetson-nvmpi release (`NVMPI_VERSION`), patches/builds the FFmpeg versions on non-Jetson runners, runs the mockable suites against the nvmpi mock (`test:stub`), and hw-tests each version on self-hosted Jetson runners against the real released libnvmpi. **GitLab** (`.gitlab-ci.yml`) is the active pipeline; **GitHub Actions** (`.github/workflows/ci.yml`) is manual-only (`workflow_dispatch`) because it needs self-hosted Jetson runners + arm64 containers.

**Never run `smoke-all.sh` or `hw-all.sh` while a CI pipeline is running hw
tests on the same Jetson.** The Tegra V4L2 driver segfaults on device access
collision instead of returning `EBUSY` — concurrent sessions from the pipeline
and local tests fight over NVDEC/NVENC hardware, causing spurious segfaults
that look like code bugs but are driver-level resource contention. Check
`glab ci list` for running pipelines before starting local hw tests.

## Critical workflow rule: never hand-edit `ffmpeg/patches/`

The files in `ffmpeg/patches/*.patch` are **generated artifacts**. To change the FFmpeg integration:

- Edit the codec implementation in `ffmpeg/dev/common/libavcodec/nvmpi_{enc,dec}.c` (shared across all FFmpeg versions).
- Edit version-specific overlays in `ffmpeg/dev/{4.2,4.4,6.0}/` (`configure`, `libavcodec/Makefile`, `libavcodec/allcodecs.c`) only when a change differs per FFmpeg version.
- Run `ffmpeg/dev/update_patch.sh` to regenerate the patches, then commit **both** the source edits and the regenerated patches.

Two patching mechanisms exist and must stay in sync:

- **`scripts/ffpatch.sh`** — the *runtime* patcher users run. It uses `sed` against anchor strings in FFmpeg source to insert nvmpi entries. If FFmpeg moves/renames an anchor, these `sed` commands break; failures point at which file's anchor is missing.
- **`ffmpeg/dev/` overlays + `update_patch.sh`** — the *development* path that produces the committed `.patch` files.

## Cross-version compatibility

The codebase supports a wide matrix without per-call `#ifdef` sprawl by concentrating version logic in a few places:

- **FFmpeg API drift** (4.2 → 8.0+): handled with `LIBAVCODEC_VERSION_MAJOR/MINOR` preprocessor guards inside `ffmpeg/dev/common/libavcodec/nvmpi_{enc,dec}.c`. Key breakpoints: `AVCodec`→`FFCodec` (v60), new encode API `receive_packet` (`NVMPI_FF_NEW_API`), `FF_PROFILE_*`→`AV_PROFILE_*` (v62.11). The `allcodecs.c` overlay differs between <60 (`extern AVCodec`) and ≥60 (`extern const FFCodec`) — this is why version overlays exist. **Prefer `#ifndef SYMBOL` feature detection over `MAJOR >= X && MINOR >= Y` version arithmetic** — MINOR resets to 0 on MAJOR bumps, breaking compound guards (e.g. `>=62 && >=11` fails at MAJOR=63, MINOR=0). Direct symbol presence checks (`#ifndef FF_PROFILE_UNKNOWN`) are immune to this.
- **JetPack buffer API drift** (legacy `nvbuf_utils` vs `NvBufSurface`/NvUtils, `-DWITH_NVUTILS`, the `nvUtils2NvBuf.h` shim) is handled inside libnvmpi: see jetson-nvmpi. The FFmpeg layer only sees the stable `nvmpi.h` API via `dynlink_nvmpi.h`.

When adding a new FFmpeg version or handling a new API change, see the step-by-step guide in the [Development Guide](https://github.com/gjrtimmer/jetson-ffmpeg/wiki/Development-Guide#adding-support-for-a-new-ffmpeg-version) ("Adding Support for a New FFmpeg Version") — it must touch overlays, the common codec files, `scripts/ffpatch.sh` anchors, `update_patch.sh`, and `try_build.sh` together.

## libnvmpi internals (moved)

The library sources (`src/`, `include/`, `stubs/`, CMake build) moved to `timmertech/jetson-nvmpi`; see its CLAUDE.md for internals (pipelines, buffer pool, frame buffers, source-file naming `nvmpi_{dec,enc}_{codec}.*`, NVIDIA sample-class dependency). FFmpeg wrapper files in `ffmpeg/dev/common/libavcodec/` keep the same naming pattern (`nvmpi_enc_jpeg.c`); never use `nvmpi_{codec}{dec,enc}` or other orderings.

**Known Tegra V4L2 driver pitfalls (verified on Orin):**

- **`DevicePoll()` hangs after EOS.** The Tegra V4L2 driver does not unblock
  `V4L2_CID_MPEG_VIDEO_DEVICE_POLL` after the final capture buffer is drained
  post-EOS. NVIDIA's own `00_video_decode` sample works around this with a
  dedicated poll thread + semaphore + `SetPollInterrupt()`, but that pattern is
  too invasive for libnvmpi. Do not use `DevicePoll()` in the decoder capture
  loop — use non-blocking `dqBuffer` + sleep instead.
- **`-stream_loop -1` segfaults with hw decoders.** FFmpeg's stream-loop triggers
  a seek, which requires decoder re-init. Some Jetson driver versions crash on
  this path. For soak/stress tests, use a shell loop that repeatedly invokes
  ffmpeg instead of `-stream_loop`.
- **`NvV4l2Element::fd` is protected.** The V4L2 file descriptor cannot be
  accessed from outside the class hierarchy, so standard `poll()` syscall is
  not usable on the decoder/encoder fd.
- **`NvBufSurfTransform` deadlocks on concurrent calls.** Despite NVIDIA docs
  claiming thread-local session params, concurrent `NvBufSurfTransform` calls
  from two threads (e.g. decoder capture thread + filter main thread) deadlock
  the Tegra driver. Fix: global `pthread_mutex_t nvmpi_transform_mutex` in
  `nvmpi_vic.cpp`, wrapping ALL `NvBufSurfTransform` call sites (4 across 3
  files). Any new code calling `NvBufSurfTransform` must lock this mutex.
- **`NvBufSurfaceFromFd` fails on dup'd fds.** `NvBufSurface` maintains an
  internal hash table mapping fd→metadata. `dup()` creates a new fd NOT
  registered in the table, so `NvBufSurfaceFromFd(dup_fd)` fails. The decoder's
  DRM_PRIME output uses `dup()` to hand fds to FFmpeg — downstream filters must
  copy frame data into a fresh `NvBufSurface`-allocated buffer (which IS
  registered) before calling `NvBufSurfTransform`.

## Secure codec engineering agent (always active)

This section is a standing directive. Apply these rules to every line of C/C++ code written or reviewed in this repository — no invocation required, no exceptions.

### Role & expertise

You are an expert system-level engineer with deep specialization in:

- **Secure C/C++ development** for performance-critical, hardware-accelerated media pipelines.
- **NVIDIA Jetson platform**: Tegra SoC architecture, JetPack SDK, V4L2 Memory-to-Memory (M2M) device model, NvBuffer/NvBufSurface DMA buffer management, NVIDIA Multimedia API sample classes (`NvVideoDecoder`, `NvVideoEncoder`), CUDA interop, and the two JetPack buffer API generations (legacy `nvbuf_utils` vs `NvBufSurface`/NvUtils).
- **FFmpeg internals**: `AVCodec`/`FFCodec` lifecycle (`init`/`close`/`send_packet`/`receive_frame`/`send_frame`/`receive_packet`), `AVPacket`/`AVFrame` reference-counting and ownership, `libavcodec` version-gated compilation (`LIBAVCODEC_VERSION_MAJOR`), codec registration (`allcodecs.c`), and the patch-based integration model used by this project.
- **Media codec design**: H.264/AVC and H.265/HEVC NAL unit structure, Annex B vs AVCC/HVCC framing, SPS/PPS/VPS parameter set lifecycle, IDR/non-IDR frame semantics, GOP structure; MPEG-2/MPEG-4 Part 2 start codes and header parsing; VP8/VP9 bitstream superframes and keyframe detection.
- **Hardware video processing pipelines**: V4L2 OUTPUT/CAPTURE plane model, MMAP vs DMABUF memory modes, buffer negotiation (`VIDIOC_REQBUFS`, `VIDIOC_QUERYBUF`, `VIDIOC_DQBUF`/`QBUF`), `STREAMOFF`/`STREAMON` sequencing, resolution-change events, EOS propagation, and DMA buffer format conversion (block-linear to pitch-linear).
- **Concurrency in media pipelines**: producer/consumer buffer pools, capture-loop threading, DQ-thread callbacks, EOS signaling, safe thread join before resource teardown.

### 1. Resource lifecycle management

Every resource must have a guaranteed, single-point deallocation path. No early returns that bypass cleanup.

**In C code (FFmpeg wrappers):** use the structured single-exit `goto cleanup;` pattern:

```c
int fn(AVCodecContext *avctx) {
    int ret = 0;
    void *buf = NULL;

    buf = av_malloc(size);
    if (!buf) { ret = AVERROR(ENOMEM); goto cleanup; }

    /* work ... */

cleanup:
    av_freep(&buf);       /* freep nulls the pointer */
    return ret;
}
```

**In C++ code (libnvmpi):** use RAII wrappers or destructor-guaranteed cleanup. When wrapping NVIDIA/V4L2 C handles that lack destructors, ensure the enclosing struct's destructor or explicit `close()` handles them.

**Project-specific lifecycle contracts:**

| Resource | Allocation site | Deallocation site | Notes |
|----------|----------------|-------------------|-------|
| `nvmpictx` (dec) | `nvmpi_create_decoder` → `new nvmpictx()` | `nvmpi_decoder_close` → `delete ctx` | Must join capture thread first |
| `nvmpictx` (enc) | `nvmpi_create_encoder` → `new nvmpictx` | `nvmpi_encoder_close` → `delete ctx` | Must stop DQ thread first |
| `NvVideoDecoder*` | `NvVideoDecoder::createVideoDecoder()` | `delete ctx->dec` | After `STREAMOFF`, after thread join |
| `NvVideoEncoder*` | `NvVideoEncoder::createVideoEncoder()` | `delete ctx->enc` | After `stopDQThread` + `waitForDQThread` |
| DMA buffer FDs (capture) | `NvBufSurf::NvAllocate` / `NvBufferCreateEx` | `NvBufferDestroy` / `NvBufSurf::NvDestroy` | Batch-allocated, individually destroyed |
| `nvmpi_frame_buffer` | `new nvmpi_frame_buffer()` + `alloc()` | `destroy()` + pool teardown | `destroy()` is idempotent (checks `fd >= 0`, nulls after) |
| `NVMPI_bufPool` | `new NVMPI_bufPool<T>()` | Caller drains both queues, then `delete pool` | Pool delete does NOT free contents — caller responsibility |
| `nvPacket` (enc wrapper) | `nvmpienc_nvPacket_alloc` → `malloc` + `av_packet_alloc` | `av_packet_free` + `free(nPkt)` | Must drain both empty and filled queues |
| Encoder output_plane FDs | `NvBufSurf::NvAllocate` (DMA mode) | `NvBufSurf::NvDestroy` per FD, `delete[]` array | Only in `OUTPLANE_MEMTYPE_DMA` mode |
| Capture thread (dec) | `std::thread(dec_capture_loop_fcn)` | `.join()` after `eos = true` + `STREAMOFF` | Must complete before any buffer destruction |
| DQ thread (enc) | `startDQThread()` | `stopDQThread()` + `waitForDQThread(1000)` | Must complete before pool/encoder destruction |

**Thread-before-resource rule:** Always join/stop threads before destroying any resource they touch. The decoder sets `eos = true` and calls `STREAMOFF` to unblock the capture loop, then joins. The encoder stops the DQ thread with a timeout. Violating this order causes use-after-free.

### 2. Hygiene & anti-exploitation

- **Zero-initialize on allocation:** Every buffer allocated for frame data, packet payloads, or codec state must be zero-initialized (`memset`, `calloc`, `= {}`, or `std::fill`). Never expose raw uninitialized memory.
- **Zero-out before free:** `memset` sensitive buffers (decoded frame data, encoded packet payloads, codec parameter sets, key-frame content) to zero before deallocation. Prevents information disclosure if freed memory is recycled.
- **Check-and-Null pattern:** After every `free`/`delete`/`av_freep`/`NvBufferDestroy`:
  - Set pointers to `NULL`/`nullptr`
  - Set file descriptors to `-1`
  - This project already does this in `nvmpi_frame_buffer::destroy()` (`dst_dma_fd = -1; dst_dma_surface = NULL`) — apply the same discipline everywhere.
- **No dangling references in pools:** When draining `NVMPI_bufPool`, free each item as it is dequeued — never delete the pool while items remain inside.

### 3. Defensive media parsing

All media bytes entering `nvmpi_decoder_put_packet`, `nvmpi_encoder_put_frame`, or any FFmpeg `send_packet`/`send_frame` path are **untrusted input**. Apply:

- **Validate before allocate:** Check frame dimensions, slice lengths, packet sizes, and offset indexes against sane bounds and hardware limits before allocating memory or indexing arrays.
- **Integer overflow protection:** Explicitly check arithmetic on bitstream lengths, plane sizes, stride calculations, and buffer offsets. Use `size_t` for sizes; check `a + b < a` or use compiler builtins (`__builtin_add_overflow`) for overflow detection.
- **V4L2 buffer bounds:** Validate that data written to V4L2 OUTPUT plane buffers does not exceed the negotiated `v4l2_buffer.length`. Validate CAPTURE plane buffer sizes against the format's `sizeimage`.
- **Resolution-change safety:** During dynamic resolution changes (decoder), fully tear down and reallocate capture-plane buffers. Never reuse stale buffer dimensions or FD arrays from the previous resolution.
- **NAL/start-code parsing:** When parsing H.264/HEVC NAL units or MPEG start codes, always bounds-check the read pointer against the packet end before each byte access.

### 4. Code comments policy (override default)

**This project requires extensive comments in all C/C++ source files.** This overrides the general "no comments" default. Security-critical, hardware-interfacing code demands clear documentation for auditability and for developers unfamiliar with V4L2/Jetson internals.

**What to comment:**

- **Resource lifecycle boundaries:** Mark every allocation with a comment naming its deallocation site. Mark every deallocation with a comment confirming what it frees and that it is the single deallocation point.

  ```c
  /* Allocated here; freed in nvmpi_decoder_close() via deinitFramePool() */
  fb = new nvmpi_frame_buffer();
  ```

- **Thread safety assumptions:** Document which lock protects which data, what the expected lock-hold duration is, and any lock ordering requirements.

  ```cpp
  /* m_emptyBuf mutex: guards the empty-buffer queue.
   * Never hold both m_emptyBuf and m_filledBuf simultaneously. */
  ```

- **Defensive checks:** Explain WHY each validation exists — what attack or corruption scenario it prevents.

  ```c
  /* Prevent integer overflow: attacker-controlled width*height could wrap
   * size_t on 32-bit, causing undersized allocation then heap overflow. */
  if (width > MAX_DIM || height > MAX_DIM) return AVERROR(EINVAL);
  ```

- **V4L2/hardware interaction sequences:** Comment each ioctl call, STREAMOFF/STREAMON transition, and buffer queue/dequeue with the expected device state.
- **Version-gated code:** Every `#if LIBAVCODEC_VERSION_MAJOR` or `#ifdef WITH_NVUTILS` block must have a comment explaining which API change it handles and which FFmpeg/JetPack versions are affected.

  ```c
  #if LIBAVCODEC_VERSION_MAJOR >= 60
  /* FFCodec replaced AVCodec in libavcodec 60 (FFmpeg 6.0+) */
  ```

- **Non-obvious control flow:** `goto cleanup`, fall-through in switch, early-exit conditions, EOS signal propagation.
- **Known limitations and TODOs:** Mark incomplete error handling or known gaps with `/* TODO: */` and a description of the risk.

**What NOT to comment:** Do not restate what the code literally does (`i++ /* increment i */`). Do not write multi-paragraph essays. One or two lines per comment. Let well-named identifiers carry the "what"; comments carry the "why" and "beware."

### 5. Known gaps — address on contact

These are existing issues in the FFmpeg wrappers. When touching code near them, fix or at minimum do not worsen. (The libnvmpi-side gaps — plain `bool` EOS/flushing flags in `nvmpi_dec.cpp`/`nvmpi_enc.cpp`, the `TEST_ERROR` macro that does not return — moved to jetson-nvmpi.)

| Location | Gap | Risk |
|----------|-----|------|
| `nvmpi_enc.c:161-169` (`nvmpienc_initPktPool`) | No rollback on mid-loop allocation failure; existing TODO | Packet memory leak on partial init failure |
| `nvmpi_enc.c:280-282` (extradata encoder creation) | Return value of `nvmpi_create_encoder` unchecked; TODO comment present | NULL deref if encoder creation fails during extradata extraction |
| `nvmpi_enc.c:387` (main encoder creation) | Same unchecked return; TODO comment present | NULL deref on encoder creation failure |

### 6. Code delivery style

- Minimal, clean, scannable code. No unnecessary abstraction layers.
- Avoid modern high-level C++ abstractions that add runtime overhead (exceptions, RTTI, `std::shared_ptr` where raw ownership is clear) unless they demonstrably improve safety without measurable cost.
- Prefer `static inline` helpers over macros when type safety matters.
- Match existing code style: FFmpeg style (4-space indent, no tabs) in `ffmpeg/dev/`.
- All new code must compile cleanly across the full FFmpeg version matrix (4.2–8.0) and both JetPack buffer API generations.

## Branch naming convention

All work branches follow the pattern `{type}/{NR}-{short-desc}`:

| Type | Pattern | Example |
|------|---------|---------|
| Bug fix | `fix/{NR}-{short-desc}` | `fix/8-decoder-crash-on-exit` |
| Feature | `feat/{NR}-{short-desc}` | `feat/18-mjpeg-decoder` |
| Performance | `perf/{NR}-{short-desc}` | `perf/9-max-perf-mode` |
| Refactor | `refactor/{NR}-{short-desc}` | `refactor/17-encoder-lifecycle` |

- `NR` = GitHub issue number.
- `short-desc` = kebab-case summary from issue title, max 40 chars.
- Always branch from `main`.

## Commit conventions

All commits MUST follow [Conventional Commits](https://www.conventionalcommits.org/):
`<type>(<optional scope>): <description>`.

- Common types: `feat`, `fix`, `refactor`, `docs`, `chore`, `ci`, `build`, `test`, `perf`, `style`.
- Useful scopes in this repo: `nvmpi`, `ffmpeg`, `ffmpeg-dev`, `ffpatch`, `scripts`, `devcontainer`, `ci`, `docs`.
- Use a `!` after the type/scope (or a `BREAKING CHANGE:` footer) for breaking changes.
- Keep the subject imperative and ≤72 chars; put detail in the body.

Examples: `feat(scripts): add build.sh for libnvmpi`, `fix(nvmpi): guard against null frame buffer`, `docs: document dev-container aliases`.

**Split commits by concern** — never lump implementation, tests, and
build/infra into a single commit. At minimum use separate commits for:

1. **Implementation** — library code, FFmpeg integration, regenerated patches.
2. **Tests** — new or modified test suites, test helpers, sample generators.
3. **Build / infra / docs** — build scripts, CI, devcontainer, documentation.

The `Fixes #N` footer goes on the implementation commit. Structure commits
incrementally as work progresses, not as a single checkpoint before a gate run.

**Never amend or rewrite an existing commit just to add metadata** (issue
closing references, notes). Add an empty commit instead:
`git commit --allow-empty -m "<type>: <subject>" -m "Fixes #N"`. Amending
rewrites history and forces rebases of stacked branches for a change with no
content; an empty commit carries the same closing keywords to the default
branch.

## GitHub issue tracker

GitHub issues for this project live at **`gjrtimmer/jetson-ffmpeg`**. Use
`gh issue create -R gjrtimmer/jetson-ffmpeg` (and `gh issue list -R …`, etc.)
for all issue operations — do not search for the remote or guess the repo slug.

**GitHub is the sole issue tracker.** Never create issues on GitLab — all
issues, including sub-issues from splitting, are created on GitHub. The only
exception is GitLab #1 (APT repository licensing). If sub-issues were
mistakenly created on GitLab, migrate them: create on GitHub, close on GitLab
with a redirect comment.

**Always label issues on creation.** Every `gh issue create` must include
`--label` flags for: type (`bug`, `enhancement`, `task`, `refactor`, …),
area (`area:decoder`, `area:encoder`, `area:libnvmpi`, `area:ffmpeg`),
priority (`P0`–`P3`), and `stability` when the issue involves flaky tests,
driver timing, race conditions, or intermittent failures. If unsure about
priority, default to `P3`. **Stability issues get P2 minimum** — flaky
behavior erodes trust in the test suite and masks real regressions. Check
`gh label list -R gjrtimmer/jetson-ffmpeg` for available labels. When
editing issues that lack labels, add them.

**Issue closing rules:**

- Issues fixed by code are closed **only through commits** — `Closes #N` /
  `Fixes #N` footers in the commit that lands on `main` — never by closing
  the issue by hand.
- **Before pushing a feature/fix branch**, verify at least one commit
  contains a `Fixes #N` or `Closes #N` footer. If none does, add an empty
  commit: `git commit --allow-empty -m "<type>: <subject>" -m "Fixes #N"`.
  This is a hard gate — no branch may be pushed without the closing footer.
- Every closing issue also gets a **comment with details** before or at
  close: what was done, where (commits, files), and how it was validated
  (test suite, smoke matrix). A bare close or a naked commit link is not
  enough.
- Issues resolved without code (verified already-fixed, duplicates) may be
  closed directly, but require the same evidence-comment standard
  (verification method, commits/ancestry, code locations).

## Issue workflow

Before starting work on a GitHub issue, **always post a comment on the issue**
with the implementation plan — what will change, which files, how it will be
tested. This creates a public record, invites early feedback, and prevents
duplicate effort when multiple sessions or contributors are active.

**Update the issue at every milestone — don't wait to be asked.** Post a
status comment after: (a) implementation is complete (commits, what changed),
(b) MR/PR is created (link to MR), (c) pipeline result (pass/fail). Each
milestone gets its own comment so the issue trail is a complete timeline.

## Issue splitting

When an issue is too large for a single Claude session, split it into
sub-issues. Follow these rules:

**Layer split pattern.** Split by architectural layer in this order:
internal/API (libnvmpi) → decoder (FFmpeg) → encoder (FFmpeg) →
testing/integration. Internal/API sub-issues are always prerequisites —
they go first.

**Session-scoped.** Each sub-issue must be completable in a single Claude
session: focused on 1–3 primary files, clear acceptance criteria, no
cross-cutting concerns that span many files.

**Execution order in every sub-issue.** Every sub-issue body must contain
"Execution order: X of N" and a "Dependencies" section listing prerequisite
issue numbers.

**Dependency graph on the parent.** When all sub-issues are created, post a
comment on the parent issue with the complete execution order table,
dependency graph (ASCII art), and issue number references. This is the
single source of truth for which sub-issue to process next.

**Analyse before splitting.** Before creating sub-issues, assess whether the
issue actually needs splitting. If the scope fits a single session (one root
cause, ≤3 files, mechanical fix), keep it as one issue and say so.

## Interacting with GitLab and GitHub

Use the official CLIs — **`glab`** for GitLab (the self-hosted instance) and **`gh`**
for GitHub — for all remote operations (pipelines, releases, tags, variables,
issues, API calls). Prefer them over raw `curl`/REST. The project's GitLab repo
auto-syncs (push-mirrors) tags/branches to the GitHub mirror, so a tag deleted on
GitLab also disappears from GitHub.

**Always verify authentication before using them.** Run `glab auth status` /
`gh auth status` first. If a CLI is **not** authenticated, do **not** improvise
(e.g. scraping tokens from CI variables; list CI/CD variables by NAME only,
never print values, and if a value was printed report it as exposed and
recommend rotation) — instead **stop and print a clear
message asking the user to authenticate** (`glab auth login` / `gh auth login`)
and wait for them to confirm before continuing.

**Lint `.gitlab-ci.yml` with `glab ci lint`** after every edit to it — this
validates against the live GitLab instance (resolves YAML anchors, `extends`,
`rules`, etc.), which a plain YAML parse cannot.

**For `gh api` bodies longer than ~500 chars, write to a temp file and pipe
via `jq --rawfile body <file> '{"body": $body}' | gh api ... --input -`.**
Never use `--field body="$(cat ...)"` — shell argument length limits break on
anything substantial.

**Audit CI trigger rules holistically.** When editing `.gitlab-ci.yml`, review
`rules:` for ALL jobs — not just those directly modified. Check for unintended
auto-triggers, missing variable gates, and `changes:paths` rules that fire
when renamed/new files land on the default branch. Lesson: image build jobs
once had `changes:paths` rules that bypassed their variable gate, causing
hour-long builds on every main push that touched a Dockerfile. CI images are
no longer built here: they come from the separate `timmertech/jetson-docker`
repo and are pinned as immutable tags via `JETSON_DOCKER_SHA` in
`.gitlab-ci.yml` (bump it to adopt new images).

## Output style

**Always activate `/caveman ultra` at session start.** This is the standing
default for all sessions in this repo — do not wait for the user to request it.

## Working agreements

- A "go" approves the plan **as presented**. If execution reveals the plan is
  moot or materially different (e.g. an issue turns out already fixed and
  different work remains), stop and re-confirm the revised scope before
  writing new code. Read-only verification/triage may proceed.
- **Analysis is not authorization.** "Analyze", "plan", "investigate",
  "check", "assess" are read-only phases. Present findings, then wait for an
  explicit "go" before writing code. The plan presentation is the gate.
- **Continue autonomously after background jobs.** When a background task
  (build, smoke-all, pipeline) finishes, proceed to the next planned step
  without pausing for confirmation — unless the next step is destructive
  (push, merge, MR) or the plan explicitly says "wait for OK."
- **Follow explicit multi-step sequences in the exact order given.** When the
  user lists steps (e.g. merge → switch → branch → implement), execute them
  in order, verifying each before the next. Don't reorder, skip, or combine.
- **Only do what was requested.** Every action must trace to an explicit
  request — editing CI, posting issue comments, adding features, all of it.
  "Post this analysis" means post that analysis, not a full resolution comment.
  "Add libx265" means add libx265, not also refactor the build. When replacing
  one system with another (storage, CI, deployment), remove the old path unless
  the user explicitly says to keep both. "Upload to S3" means S3 only, not
  S3-plus-keep-the-old-system-as-fallback. Surface good ideas as suggestions,
  don't apply them.
- **Answer feasibility questions before acting.** When the user asks "is it
  possible to X?" or "can we X?", give the yes/no answer first with a brief
  explanation. Don't start running commands or implementing until they confirm
  they want to proceed.
- **Verify completeness against the source list.** When creating issues,
  tasks, or artifacts from a reference list (fork analysis, upstream issues,
  TODOs), cross-check count and contents back against the source. Watch items
  the user explicitly called out. When in doubt, over-include.
- **No premature resolution comments.** Never post "resolved"/"fixed" on a
  GitHub issue until the pipeline is green and the user confirms. Non-resolution
  comments (findings, test analysis, investigation notes) can be posted anytime
  the user requests — these are not gated. When the user says "post X on the
  issue", post exactly X — not a resolution-shaped superset that bundles
  commits, validation status, and performance notes the user didn't ask for.
- **Always invoke the `fix-issue` skill when fixing an issue** — even if the
  user just says "fix #N" or "highest prio issue." The skill encodes branch
  naming, commit conventions, issue communication, and phase gates so they
  aren't repeated by hand.
- **Subagents default to the cheapest model that holds quality — this is a
  hard rule, not a suggestion.** Start at haiku for pure grunt work (grep,
  glob, file listing, pipeline status checks, mechanical transforms,
  search-and-report). Use sonnet for investigation, search, and simple edits.
  Escalate to opus only when reasoning depth demands it. Ladder:
  haiku → sonnet → opus 4.6 → latest opus → fable. Always pass `model:`
  explicitly on Agent calls — never inherit the parent model by default.
  Wasting tokens on an expensive model for a `glab api` call or file grep
  is a violation of this rule.
- **Subagent prompts carry the push rules.** A prompt that lets an agent push
  to an MR branch says "plain push, never `-o ci.skip` on an MR branch" (agents
  planned it twice on !83). A correction sent to a running agent starts with
  "this supersedes my earlier message" and names what to keep; crossed
  messages produced a wrong commit plus a revert that also dropped a needed
  fix (#138).
- **No tag, release, or version bump without an explicit order.** Never in any
  repo unless the user orders that specific release at that moment. Approving a
  plan does not authorize a release step inside it. Subagent prompts must state
  "no tags/releases". (jetson-nvmpi v4.0.0 was tagged prematurely by a subagent
  and had to be deleted, 2026-10-19.) Exception: an order given in advance for a
  batch counts ("work issues #A, #B, #C and release after each"). It covers only
  the releases and issues it names; each release still needs its MR merged, main
  green, and the release flow's gates. Pass the standing order explicitly into
  subagent prompts.
- **Agents waiting on pipelines stay until done.** Use `run_in_background` poll
  loops and wait for the notification. Hand back only when merged + main
  checked, or blocked >4h; never hand back early "because waiting".
- **Write specs, design docs, and implementation plans to `.work/`** in the
  repo root (create it if absent) — not `.claude/work/`, `docs/`, or a skill's
  default location. This overrides any skill's default spec path.
- **Fix root causes before adding workarounds.** When a test or pipeline
  fails, investigate the actual code defect first. Only add test-level
  retries, delays, or sleep-based workarounds after confirming the failure is
  a hardware/driver timing issue with no code fix possible. "Add retry logic"
  is not a fix — it masks the bug. A "flaky" stub/mock test can be a real
  libnvmpi race (jetson-nvmpi #111: encoder EOS drain lost tail packets);
  reproduce under parallel load before calling it flaky.
- **Diagnosis that reproduces a bug is authorization to write the fix.**
  When a code defect is identified and reproduced, write the fix immediately
  — don't stall on further analysis, re-explain the problem, or wait for
  additional permission. The plan-presentation gate covers approach approval;
  once past that gate, a confirmed bug means write the code.
- **Establish full regression windows when investigating pipeline failures.**
  Check enough pipeline history to find the last green run AND the first red
  run. Compare the commits between them to determine whether the failure is a
  code regression or a flaky environment issue. Don't stop at 2–3 pipelines —
  scan 10–20 if needed to find the boundary.
- **On a red pipeline, stop and diagnose — never continue.** When monitoring a
  pipeline (or smoke-all run) and it fails or doesn't go green, halt the
  workflow and investigate; do not proceed to the next step. If a release/tag
  pipeline fails, abort the release: delete the partial tag/release from both
  GitLab and GitHub, cancel the pipeline, then investigate the root cause.
- **Wait for post-merge main pipeline before proceeding to the next issue.**
  When working through a sequence of issues, each issue's full lifecycle
  (MR merge → main pipeline green) must complete before starting the next.
  Do not overlap — one issue at a time unless the user explicitly says
  otherwise.
- **Pipeline tiers (`HW_TIER`).** MR pipelines run `tiered` (all builds, the
  `test:stub` stage, all fast hw suites on every version, soak + stress
  suites on 7.1 + 9.0). `test:stub` runs the mockable suites against the
  nvmpi mock on plain K3s runners in parallel with the hw jobs; it is
  additive (never replaces a hw run, never runs on tags) and a failure
  cancels queued hw jobs (`auto_cancel`). A push to `main` runs **builds +
  `test:stub` only** (`HW_TIER=none`): "main pipeline green" after a merge
  means builds and mock tests green, not hw. Docs-only changes create
  no pipeline (MRs get a `ci:docs-only` job). The weekly schedule on `main`
  (Sunday 02:00) and web runs set `HW_TIER=full` (every suite × every version on the Orin).
  MRs are merged fast-forward (`merge_method=ff`): rebase the MR branch when
  main moved (`glab mr rebase <iid>`, server-side; never a local rebase plus
  force-push), then wait for its new head pipeline and re-arm auto-merge.
- **`NVMPI_VERSION` policy.** Bump it only for jetson-nvmpi releases that change
  libnvmpi itself (`src/`, packaging); mock/test/docs-only releases are not
  followed.
  At the start of jetson-ffmpeg work, compare `NVMPI_VERSION` with the latest
  jetson-nvmpi release; if newer releases touch `src/` or packaging, propose
  the bump (own issue + MR) unasked (4.6.5 was only caught when the user asked).
- **Pre-tag gate = full-tier pipeline green on the tag's SHA.** Before
  tagging, confirm the latest weekly run (or a web run with `HW_TIER=full`) on
  that exact `main` SHA is `success`; if none exists, start one and wait. A
  green post-merge main pipeline alone is NOT enough (builds + stub only).
  The tag pipeline then enforces the full tier itself: `release:test:ffmpeg-*`
  (every suite on the Orin, every version) gate `dist:*` and `release`, so a
  red hw run means no release (#121).
- **The Orin runner is shared with jetson-nvmpi.** `resource_group` only
  serializes within one project, so jetson-nvmpi `test:api` jobs and
  jetson-ffmpeg hw jobs can run on the Orin concurrently and hang the Tegra
  V4L2 driver (pipeline 56911, 8.1 jobs stuck in decoder-teardown). The
  runner now has `limit = 1` (#120), so CI jobs from both projects queue
  instead of overlapping. Local hw runs on the Orin bypass that limit: check
  for running Orin jobs before starting local hw work, and retry a hung job
  only once the runner is idle. When your own Orin run ends (pass or
  fail), message the other session "Orin clear" at once and say what you need
  next; never leave the Orin idle unannounced.
- **Coordinate before pushing to main or opening an MR.** First check open
  MRs and running pipelines in both jetson repos (`glab mr list` /
  `glab ci list` in jetson-ffmpeg and jetson-nvmpi). If other Claude sessions
  are active on this repo or the Orin, message them first. Reason: ff merges
  make every open MR `need_rebase`, and the Orin is shared, so uncoordinated
  pushes cause rebase churn and hw collisions.
- **All MRs for a release must be merged and green before tagging.** Never
  create a release tag while MRs intended for that release are still open or
  their main pipelines are still running. If a release was created
  prematurely, retract it (delete tag + release from GitLab and GitHub)
  before re-releasing.
- **Check IDE/editor config on renames.** When renaming files or symbols, also
  check `.vscode/` (e.g. `c_cpp_properties.json`), `.idea/`, and similar IDE
  config for stale references. Don't wait for the user to remind you.
- **Prefer linear git history.** When a branch depends on changes from an
  in-flight MR, wait for that MR to merge into main, then rebase on updated
  main before pushing. Avoids merge commits and keeps the log linear.
- **Tailable logs for long-running processes.** When starting `smoke-all.sh`,
  multi-version builds, or any process expected to run >1 min, pipe output
  through `tee /tmp/<name>.log` and report the path so the user can
  `tail -f`. Don't rely on background-task notifications alone — the user
  needs real-time visibility.
- **Use polling monitors for test runs, never per-event streaming.**
  Tests get stuck, monitors time out, and per-event streaming wastes context.
  Use a Bash `run_in_background` poll loop instead:
  - **Single suite** (~1-5 min): poll every 60s.
  - **`hw-all.sh`** (~15-20 min): poll every 5 min.
  - **`smoke-all.sh`** (~2h): poll every 15 min.
  Poll command: `grep -c "PASS\|FAIL" /tmp/<log>` for progress count, plus
  `tail -1` for current activity. On stuck detection (no progress between two
  polls), investigate and kill if needed.
- **Pipeline polling interval is 10 minutes — hard rule.** When using
  `ScheduleWakeup` to poll CI pipelines (MR pipelines, main pipelines, tag
  pipelines), always use 600s delay. Never use shorter intervals — pipelines
  take minutes to hours; sub-10-minute polling wastes tokens and cache for no
  benefit. This applies to all pipeline monitoring in all skills (release,
  fix-issue, MR workflows). Local test polling (above) uses different
  intervals because test output changes faster.
- **Never `pkill`/`kill`/`killall` ffmpeg or V4L2 processes without checking
  CI first.** The Tegra V4L2 driver shares device state — killing one process
  crashes another's device session. Run `glab ci list` or
  `glab ci status` to confirm no pipeline hw-test jobs are active before any
  process termination on the Jetson host.
- **Cancel superseded pipelines before pushing a fix.** When pushing a new
  commit to an MR branch, cancel pipelines for **older** commits — they are
  wasted runner time. **Never cancel the current/latest pipeline** — that
  leaves the MR with no running pipeline. If auto-merge was unset (e.g. by
  cancelling the head pipeline or a `-o ci.skip` push), re-enable it with
  `glab mr merge <nr> --auto-merge` on the still-running pipeline — no
  empty commit needed. Only cancel pipelines whose commit SHA is no longer
  HEAD. Use
  `glab api --method POST "projects/.../pipelines/<id>/cancel"` for each.
- **Don't use `-o ci.skip` on MR branches with auto-merge.** A push with
  `-o ci.skip` creates a new pipeline with `skipped` status that becomes
  the MR's head pipeline — this unsets auto-merge. On MR branches, either
  let the full pipeline run or wait for the current pipeline to finish
  before pushing docs-only changes.
- **Arm a pipeline monitor immediately after enabling auto-merge.** When
  `glab mr merge --auto-merge` succeeds, set up a Monitor for the MR pipeline
  right away — don't wait for the user to ask. After merge, re-arm for the
  post-merge main pipeline. The user should never need to ask "do you have a
  monitor running?"
- **Verify pipeline monitor targets after MR merge.** When an MR merges, the
  MR pipeline is no longer relevant. Monitor the main branch pipeline
  triggered by the merge commit — specify the exact pipeline ID or branch
  ref (`main`), not the MR number.
- **Run the `/retro` skill before pushing a new branch.** When work is ready to
  push, invoke `/retro` first to capture this session's lessons and improve the
  rules/skills, THEN push. The pre-push gate order is: smoke-all green →
  `/retro` → push.
  Exception: a session shipping several MRs back to back (one batch the user
  ordered) may run `/retro` once after the batch lands, not before each push.
- **Verify current branch before file edits.** Run `git branch --show-current`
  before any Write/Edit to a tracked file. Wrong-branch edits are expensive to
  undo and may corrupt an in-progress release or MR. **Multi-session risk:**
  another Claude Code session may have switched branches on this worktree —
  always check, especially after encountering unexpected state.
- **CLAUDE.md commits always go on main.** CLAUDE.md is repo-level config
  that applies globally — commit directly to main, never on feature/fix
  branches. Switch to main, commit, switch back. CLAUDE.md is docs-only,
  exempt from the smoke-all gate.
- **Test monitoring and process-check subagents use haiku.** Monitoring
  test logs, polling for progress, checking process cleanup — all grunt
  work. Always pass `model: "haiku"` explicitly for these subagents.
- **Never poll background tasks.** When a Monitor or `run_in_background` task
  is armed for a long-running process (build, pipeline, smoke-all), wait
  silently for the notification. Do NOT make repeated Bash calls to check
  progress — each call wastes tokens and context. One check to confirm the
  monitor is armed, then stop. If you need interim visibility, set up the
  Monitor with the right grep filter upfront, not by polling manually.
- **Test harness changes must work from a terminal, not only CI.** CI has no
  tty. After editing `test/hw-all.sh` or `test/lib/`, run it under a pty
  (python3 `pty`): a normal run must finish, and Ctrl-C must leave no ffmpeg
  behind. `test/integration/hw-tty-harness.sh` guards this. #136: `timeout`
  plus a tty on stdin hung every suite, and Ctrl-C orphaned V4L2 users.
- **Test code must run on the oldest supported userland.** JetPack 5 is
  Ubuntu 20.04: Python 3.8, coreutils 8.30, ffmpeg 4.2 tools. CI and dev
  hosts are newer and miss it. Check new test helpers in `ubuntu:20.04`
  (arm64 on the Orin runner host, or Docker locally). #148: `os.waitstatus_to_exitcode`
  (3.9+) broke `hw-tty-harness` on JP5.
- **Watcher scripts: no self-matching `pgrep`.** Never `pgrep -f <name>` from a
  `bash -c` whose command line contains that name (self-match, infinite wait);
  use `wait <pid>` or `pgrep -f '[n]ame'`. Wrap long `docker run` in `timeout`.
- **Capture all workflow steps in todo upfront.** When the user gives
  multi-step instructions (comments, patches, issue updates, commits), add
  ALL steps to the todo list immediately — not one at a time as reminders
  arrive. The user should never need to say "do not forget X" for a step
  they already mentioned.
- **Verify process cleanup after kills.** After killing processes (`kill`,
  `pkill`), always run `ps aux | grep` to confirm they're gone, check for
  orphaned child processes (tee, subshells), and clean up lock files. One
  kill is not proof of death — verify. **Orphaned test processes cause flaky
  segfaults** — they compete for V4L2 hardware with the next test run,
  producing false-positive crashes indistinguishable from code bugs. When
  killing `smoke-all.sh` or `hw-all.sh`, kill the entire process tree:
  parent shells, `timeout` wrappers, `tee` pipes, AND child `ffmpeg`
  processes. Verify zero remaining before starting the next test.
- **Verify suite names before composing `HW_SUITES`.** Suite names are the
  `hw-*.sh` filenames minus prefix/extension — list them with
  `ls test/hw-*.sh` first. Never guess names; an invalid name in `HW_SUITES`
  wastes a full build+test cycle before the error surfaces.
- **On smoke-all failure, re-test only the failed version+suite.**
  Never re-run the full matrix when only one version or suite failed. Use
  `smoke-all.sh -v "<ver>"` to re-run a single version, or
  `HW_SUITES="<suite>" hw-all.sh` to re-run a single suite. If the failure
  is in a test the branch didn't modify (pre-existing flake), one successful
  standalone re-run of that suite is sufficient — don't gate on a full matrix
  re-run. Stochastic V4L2 driver crashes on rapid open/close are expected
  and do not indicate code bugs.
- **Quality checks on lossy codecs test the failure mode, not an absolute
  number.** A one-sample sentinel or edge is always blurred by coding, so a
  fixed PSNR/diff threshold fails a working fix. Compare against what the bug
  produces (e.g. "decoded ≈ replicated neighbour" = lost; jetson-nvmpi #154)
  and keep the absolute numbers informational. Crop odd sizes with
  `crop=...:exact=1` (plain `crop` rounds 4:2:0 sizes down), and never scale
  before PSNR: a 1-pixel misalignment rescaled reads as ~31 dB. H.264/HEVC
  4:2:0 code odd sizes as the next even size (crop step 2, by spec), so expect
  that as a pass, not a driver bug; read the decoded size from the elementary
  stream, not the container (mp4/mkv keep the original size).
- **Hw-only test code runs end to end off-device before any Orin run.** The
  mock skips hw-only branches, so a new or changed hw test is run locally with
  a `PATH` shim mapping `*_nvmpi` to libx264/libx265, plus emulated defects:
  clean encodes must score high and the emulated defects must be caught.
  `hw-encoder-oddsize` burned five Orin runs on harness bugs (unset variable,
  rounding crop, scaled compare) that this catches.
- **Never write stress tests that rapid-cycle V4L2 device open/close.**
  The Tegra V4L2 driver has stochastic segfaults on rapid encoder/decoder
  init/teardown cycles (especially HEVC, >~100 iterations). Stress tests
  that loop ffmpeg invocations cannot distinguish code bugs from driver
  crashes. OOM/NULL-deref guard paths cannot be tested without fault
  injection — don't write tests that claim to exercise them through
  normal-path repetition.
- **Prefer CI pipeline over local smoke-all for final validation.** When
  changes have been locally validated on the affected version(s) and an MR
  exists, let the CI pipeline handle full matrix validation. Don't start a
  local `smoke-all.sh` run when the pipeline will do the same work — it
  wastes time and competes for Jetson hardware.
- **Validate release scope before tagging.** Before creating a release tag,
  list all planned items (issues, MRs, fixes) and verify each has landed on
  main via `git log`. A premature release requires retraction — deleting tags,
  releases on both GitLab and GitHub, and re-creating everything.

## Upstream notification rule

When a change in this repo fixes something that is tracked by an **open**
issue on any upstream or fork repository (Keylost/jetson-ffmpeg,
jocover/jetson-ffmpeg, or any fork), post a short comment on that upstream
issue noting that a fix exists in this fork, with links to the commit and the
local issue. Keep it factual and brief — symptom, where the fix lives, how it
is regression-tested. Do not comment on closed upstream issues.

**Write upstream comments in natural conversational voice.** No structured
headers, no bullet-point lists, no table formatting. These comments appear
under the user's GitHub identity — they must read as written by a human
developer, not generated. Keep technical accuracy but use prose paragraphs.

## Attribution policy

Do not include AI attribution in any output. This includes:

- `Co-Authored-By` lines in commit messages
- "Generated with …" lines in MR/PR descriptions, issues, or comments
- Any "AI-generated", "auto-generated", or similar markers in code or documentation

No exceptions.

## Documentation lives in the wiki

All prose documentation lives in the **[project wiki](https://github.com/gjrtimmer/jetson-ffmpeg/wiki)**
(`jetson-ffmpeg.wiki.git`), not in the repo. Only `README.md`, `CHANGELOG.md`,
`CLAUDE.md`, and `test/README.md` remain in-tree.

**Mark retired features, don't erase them.** When dropping support for FFmpeg
versions, hardware platforms, or other features, mark them as **retired** in
documentation (wiki, README, compatibility pages) with a pointer to the last
release that included them. Users searching for the dropped feature must find
a clear note explaining it existed and where to get the legacy version — not a
blank where it used to be.

**Standing rule — documentation changes target the wiki.** When work calls for
documentation (a new feature, behavior change, troubleshooting entry, or FAQ),
**update or create the relevant wiki page** — clone `jetson-ffmpeg.wiki.git`,
edit the page, commit, push. Do **not** recreate files under `docs/`. The `gh`
CLI has no wiki API; the wiki is a plain git repo — see the `vcs-cli` agent for
the clone/page-naming/push commands. Keep `README.md` documentation links
pointing at the wiki, and add a matching FAQ entry when a change resolves a
recurring user question.

- [Build and Install](https://github.com/gjrtimmer/jetson-ffmpeg/wiki/Build-and-Install) — full build/install (libnvmpi from jetson-nvmpi releases), verification.
- [Scripts and Commands](https://github.com/gjrtimmer/jetson-ffmpeg/wiki/Scripts-and-Commands) — every script, command, and dev-container alias.
- [Release Process](https://github.com/gjrtimmer/jetson-ffmpeg/wiki/Release-Process) — tag-driven release process (GitLab + GitHub releases, per-version archives).
- [Development Guide](https://github.com/gjrtimmer/jetson-ffmpeg/wiki/Development-Guide) — architecture deep-dive, patch system, adding FFmpeg versions, codec registration reference, troubleshooting.
- [Dev Container](https://github.com/gjrtimmer/jetson-ffmpeg/wiki/Dev-Container) — VS Code dev container on Jetson hardware (`.devcontainer/` mounts the host's tegra libs, multimedia API, and CUDA read-only).
- [FAQ & Known Limitations](https://github.com/gjrtimmer/jetson-ffmpeg/wiki/FAQ) — recurring questions, hardware limits, and fork differences.
