# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

**jetson-ffmpeg** enables hardware-accelerated H.264/HEVC/MPEG2/MPEG4/VP8/VP9 video encode/decode on NVIDIA Jetson via FFmpeg. It is built from **two distinct layers** that ship and build separately:

1. **libnvmpi** (`src/`, `include/`, `CMakeLists.txt`) — a standalone C-API shared library that wraps NVIDIA's V4L2/NvBuffer multimedia API. Installed system-wide as `libnvmpi.so`.
2. **FFmpeg integration** (`ffmpeg/dev/`, `ffmpeg/patches/`) — codec source files (`AVCodec`/`FFCodec` wrappers) that call libnvmpi and get *patched into* a vanilla FFmpeg tree, then compiled as part of FFmpeg.

FFmpeg does not depend on this repo at runtime beyond `libnvmpi.so`; the integration layer is delivered as patches users apply to their own FFmpeg checkout.

## Repository layout

- `src/`, `include/`, `CMakeLists.txt`, `stubs/` — the libnvmpi library.
- `ffmpeg/dev/` — FFmpeg patch *development* tree: shared codec sources (`common/`), per-version overlays (`4.2/`, `4.4/`, `6.0/`), and the patch-generation scripts (`update_patch.sh`, `copy_files.sh`, `try_build.sh`).
- `ffmpeg/patches/` — generated `ffmpeg<ver>_nvmpi.patch` files (artifacts; never hand-edit).
- `scripts/` — operator scripts: `build.sh` (build libnvmpi) and `ffpatch.sh` (runtime FFmpeg patcher). All scripts resolve the repo root from their own location, so they run from any working directory.
- `test/` — per-feature hardware suites (`hw-*.sh`) run by `hw-all.sh` (auto-discovery; see `test/README.md`), `gen-samples.sh` (shared sample generators), and `smoke-all.sh` (full cross-version build + hw-all). FFmpeg sources are fetched by `scripts/clone-ffmpeg.sh`.
- Prose documentation lives in the **[project wiki](https://github.com/gjrtimmer/jetson-ffmpeg/wiki)** (`jetson-ffmpeg.wiki.git`), not in-repo — the `docs/` folder has been retired. Script/alias reference: [Scripts and Commands](https://github.com/gjrtimmer/jetson-ffmpeg/wiki/Scripts-and-Commands).

Supported FFmpeg versions: 4.2, 4.4, 6.0, 6.1, 7.0, 7.1, 8.0 (libavcodec 58→62).

## Build & test commands

In the dev container these are also exposed as aliases (`build`, `ffpatch`,
`update-patch`, `try-build`, `hw-all`) — see [Scripts and Commands](https://github.com/gjrtimmer/jetson-ffmpeg/wiki/Scripts-and-Commands). There is
deliberately no `test` alias (it would shadow the shell builtin).

```bash
# Build libnvmpi (auto-detects real Jetson libs vs stubs/; --install to install)
./scripts/build.sh                 # alias: build
./scripts/build.sh --stubs         # force stubs build (off-Jetson / CI)
./scripts/build.sh --install       # build then sudo make install + ldconfig
./scripts/build.sh --ffmpeg 7.1    # also build a full FFmpeg <ver> with nvmpi
# Equivalent raw CMake build:
mkdir build && cd build && cmake .. && make -j$(nproc) && sudo make install && sudo ldconfig

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

# Full cross-version smoke test: build libnvmpi, then patch+build+hw-test EVERY version
./test/smoke-all.sh                          # all 7 versions
./test/smoke-all.sh -v "4.2 6.0 8.0"         # subset
```

There is no unit-test suite. Verification is layered: per-feature hardware suites (`test/hw-*.sh`, run by `test/hw-all.sh`; documented in `test/README.md`), the full cross-version harness (`test/smoke-all.sh`), and CI. New features/fixes ship together with the suite that guards them.

**Never push code changes without a passing `./test/smoke-all.sh` run** (7/7
matrix green). Docs-only changes are exempt and may push with `-o ci.skip`.

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
auto-merge.

**Never `rm -rf build`** — use `./scripts/build.sh --clean`; the build
directory may be shared with a concurrent build. CI compiles libnvmpi + patches/builds all seven FFmpeg versions against `stubs/` on non-Jetson runners, and hw-tests each version on self-hosted Jetson runners. **GitLab** (`.gitlab-ci.yml`) is the active pipeline; **GitHub Actions** (`.github/workflows/ci.yml`) is manual-only (`workflow_dispatch`) because it needs self-hosted Jetson runners + arm64 containers.

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

- **FFmpeg API drift** (4.2 → 8.0+): handled with `LIBAVCODEC_VERSION_MAJOR/MINOR` preprocessor guards inside `ffmpeg/dev/common/libavcodec/nvmpi_{enc,dec}.c`. Key breakpoints: `AVCodec`→`FFCodec` (v60), new encode API `receive_packet` (`NVMPI_FF_NEW_API`), `FF_PROFILE_*`→`AV_PROFILE_*` (v62.11). The `allcodecs.c` overlay differs between <60 (`extern AVCodec`) and ≥60 (`extern const FFCodec`) — this is why version overlays exist.
- **JetPack buffer API drift**: legacy `nvbuf_utils` vs newer `NvBufSurface`/NvUtils (JetPack 5+). `CMakeLists.txt` auto-detects by probing for `nvbufsurface.h`; if present it defines `-DWITH_NVUTILS` and links the surface libs. `include/nvUtils2NvBuf.h` is a compile-time shim that maps legacy `NvBuffer*` names to `NvBufSurf*` so the rest of `src/` stays API-agnostic.

When adding a new FFmpeg version or handling a new API change, see the step-by-step guide in the [Development Guide](https://github.com/gjrtimmer/jetson-ffmpeg/wiki/Development-Guide#adding-support-for-a-new-ffmpeg-version) ("Adding Support for a New FFmpeg Version") — it must touch overlays, the common codec files, `scripts/ffpatch.sh` anchors, `update_patch.sh`, and `try_build.sh` together.

## libnvmpi internals (`src/`)

- `nvmpi_dec.cpp` / `nvmpi_enc.cpp` — V4L2 decode/encode pipelines exposed through the C API in `include/nvmpi.h` (`nvmpi_create_*`, `put`/`get`, `close`).
- `NVMPI_bufPool.hpp` — thread-safe producer/consumer pool used for both decoded-frame and encoded-packet buffers.
- `nvmpi_frame_buffer.{hpp,cpp}` — DMA buffer alloc/destroy, abstracting NvUtils vs nvbuf_utils.

The CMake build also pulls NVIDIA sample classes (`NvVideoDecoder`, `NvVideoEncoder`, etc.) from `${JETSON_MULTIMEDIA_API_DIR}/samples/common/classes` — these are not vendored in this repo and must exist on the build host (or via the devcontainer mounts).

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

These are existing issues found in the codebase. When touching code near them, fix or at minimum do not worsen:

| Location | Gap | Risk |
|----------|-----|------|
| `nvmpi_enc.c:161-169` (`nvmpienc_initPktPool`) | No rollback on mid-loop allocation failure; existing TODO | Packet memory leak on partial init failure |
| `nvmpi_enc.c:280-282` (extradata encoder creation) | Return value of `nvmpi_create_encoder` unchecked; TODO comment present | NULL deref if encoder creation fails during extradata extraction |
| `nvmpi_enc.c:387` (main encoder creation) | Same unchecked return; TODO comment present | NULL deref on encoder creation failure |
| `nvmpi_dec.cpp:71`, `nvmpi_enc.cpp:89-90` | EOS/flushing flags are plain `bool`, not `std::atomic<bool>` | Data race between main thread and capture/DQ thread (undefined behavior per C++ standard; works in practice on ARM due to aligned-word atomicity, but not guaranteed) |
| `nvmpi_dec.cpp:43-47` (`TEST_ERROR` macro) | Logs error but does not return or set error code | Execution continues after allocation failure; downstream code may deref invalid buffer |

### 6. Code delivery style

- Minimal, clean, scannable code. No unnecessary abstraction layers.
- Avoid modern high-level C++ abstractions that add runtime overhead (exceptions, RTTI, `std::shared_ptr` where raw ownership is clear) unless they demonstrably improve safety without measurable cost.
- Prefer `static inline` helpers over macros when type safety matters.
- Match existing code style: 4-space tabs in C++ (`src/`), FFmpeg style (4-space indent, no tabs) in `ffmpeg/dev/`.
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

**Always label issues on creation.** Every `gh issue create` must include
`--label` flags for: type (`bug`, `enhancement`, `task`, `refactor`, …),
area (`area:decoder`, `area:encoder`, `area:libnvmpi`, `area:ffmpeg`), and
priority (`P0`–`P3`). If unsure about priority, default to `P3`. Check
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

## Interacting with GitLab and GitHub

Use the official CLIs — **`glab`** for GitLab (`gitlab.timmertech.nl`) and **`gh`**
for GitHub — for all remote operations (pipelines, releases, tags, variables,
issues, API calls). Prefer them over raw `curl`/REST. The project's GitLab repo
auto-syncs (push-mirrors) tags/branches to the GitHub mirror, so a tag deleted on
GitLab also disappears from GitHub.

**Always verify authentication before using them.** Run `glab auth status` /
`gh auth status` first. If a CLI is **not** authenticated, do **not** improvise
(e.g. scraping tokens from CI variables) — instead **stop and print a clear
message asking the user to authenticate** (`glab auth login` / `gh auth login`)
and wait for them to confirm before continuing.

**Lint `.gitlab-ci.yml` with `glab ci lint`** after every edit to it — this
validates against the live GitLab instance (resolves YAML anchors, `extends`,
`rules`, etc.), which a plain YAML parse cannot.

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
  "Add libx265" means add libx265, not also refactor the build. Surface good
  ideas as suggestions, don't apply them.
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
- **Subagents default to the cheapest model that holds quality.** Use sonnet
  for investigation, search, and simple edits; escalate to opus only when
  reasoning depth demands it. Ladder: sonnet → opus 4.6 → latest opus → fable.
- **Write specs, design docs, and implementation plans to `.work/`** in the
  repo root (create it if absent) — not `.claude/work/`, `docs/`, or a skill's
  default location. This overrides any skill's default spec path.
- **On a red pipeline, stop and diagnose — never continue.** When monitoring a
  pipeline (or smoke-all run) and it fails or doesn't go green, halt the
  workflow and investigate; do not proceed to the next step. If a release/tag
  pipeline fails, abort the release: delete the partial tag/release from both
  GitLab and GitHub, cancel the pipeline, then investigate the root cause.
- **Check IDE/editor config on renames.** When renaming files or symbols, also
  check `.vscode/` (e.g. `c_cpp_properties.json`), `.idea/`, and similar IDE
  config for stale references. Don't wait for the user to remind you.
- **Prefer linear git history.** When a branch depends on changes from an
  in-flight MR, wait for that MR to merge into main, then rebase on updated
  main before pushing. Avoids merge commits and keeps the log linear.
- **Run the `/retro` skill before pushing a new branch.** When work is ready to
  push, invoke `/retro` first to capture this session's lessons and improve the
  rules/skills, THEN push. The pre-push gate order is: smoke-all green →
  `/retro` → push.

## Upstream notification rule

When a change in this repo fixes something that is tracked by an **open**
issue on any upstream or fork repository (Keylost/jetson-ffmpeg,
jocover/jetson-ffmpeg, or any fork), post a short comment on that upstream
issue noting that a fix exists in this fork, with links to the commit and the
local issue. Keep it factual and brief — symptom, where the fix lives, how it
is regression-tested. Do not comment on closed upstream issues.

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

**Standing rule — documentation changes target the wiki.** When work calls for
documentation (a new feature, behavior change, troubleshooting entry, or FAQ),
**update or create the relevant wiki page** — clone `jetson-ffmpeg.wiki.git`,
edit the page, commit, push. Do **not** recreate files under `docs/`. The `gh`
CLI has no wiki API; the wiki is a plain git repo — see the `vcs-cli` agent for
the clone/page-naming/push commands. Keep `README.md` documentation links
pointing at the wiki, and add a matching FAQ entry when a change resolves a
recurring user question.

- [Build and Install](https://github.com/gjrtimmer/jetson-ffmpeg/wiki/Build-and-Install) — full build/install, CMake options, verification.
- [Scripts and Commands](https://github.com/gjrtimmer/jetson-ffmpeg/wiki/Scripts-and-Commands) — every script, command, and dev-container alias.
- [Release Process](https://github.com/gjrtimmer/jetson-ffmpeg/wiki/Release-Process) — tag-driven release process (GitLab + GitHub releases, per-version archives).
- [Development Guide](https://github.com/gjrtimmer/jetson-ffmpeg/wiki/Development-Guide) — architecture deep-dive, patch system, adding FFmpeg versions, codec registration reference, troubleshooting.
- [Dev Container](https://github.com/gjrtimmer/jetson-ffmpeg/wiki/Dev-Container) — VS Code dev container on Jetson hardware (`.devcontainer/` mounts the host's tegra libs, multimedia API, and CUDA read-only).
- [FAQ & Known Limitations](https://github.com/gjrtimmer/jetson-ffmpeg/wiki/FAQ) — recurring questions, hardware limits, and fork differences.
