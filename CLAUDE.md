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
- `docs/SCRIPTS.md` — reference for every script, command, and dev-container alias.

Supported FFmpeg versions: 4.2, 4.4, 6.0, 6.1, 7.0, 7.1, 8.0 (libavcodec 58→62).

## Build & test commands

In the dev container these are also exposed as aliases (`build`, `ffpatch`,
`update-patch`, `try-build`, `hw-all`) — see `docs/SCRIPTS.md`. There is
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

There is no unit-test suite. Verification is layered: per-feature hardware suites (`test/hw-*.sh`, run by `test/hw-all.sh`; documented in `test/README.md`), the full cross-version harness (`test/smoke-all.sh`), and CI. New features/fixes ship together with the suite that guards them. CI compiles libnvmpi + patches/builds all seven FFmpeg versions against `stubs/` on non-Jetson runners, and hw-tests each version on self-hosted Jetson runners. **GitLab** (`.gitlab-ci.yml`) is the active pipeline; **GitHub Actions** (`.github/workflows/ci.yml`) is manual-only (`workflow_dispatch`) because it needs self-hosted Jetson runners + arm64 containers.

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

When adding a new FFmpeg version or handling a new API change, see the step-by-step guide in `docs/DEVELOPMENT.md` ("Adding Support for a New FFmpeg Version") — it must touch overlays, the common codec files, `scripts/ffpatch.sh` anchors, `update_patch.sh`, and `try_build.sh` together.

## libnvmpi internals (`src/`)

- `nvmpi_dec.cpp` / `nvmpi_enc.cpp` — V4L2 decode/encode pipelines exposed through the C API in `include/nvmpi.h` (`nvmpi_create_*`, `put`/`get`, `close`).
- `NVMPI_bufPool.hpp` — thread-safe producer/consumer pool used for both decoded-frame and encoded-packet buffers.
- `NVMPI_frameBuf.{hpp,cpp}` — DMA buffer alloc/destroy, abstracting NvUtils vs nvbuf_utils.

The CMake build also pulls NVIDIA sample classes (`NvVideoDecoder`, `NvVideoEncoder`, etc.) from `${JETSON_MULTIMEDIA_API_DIR}/samples/common/classes` — these are not vendored in this repo and must exist on the build host (or via the devcontainer mounts).

## Commit conventions

All commits MUST follow [Conventional Commits](https://www.conventionalcommits.org/):
`<type>(<optional scope>): <description>`.

- Common types: `feat`, `fix`, `refactor`, `docs`, `chore`, `ci`, `build`, `test`, `perf`, `style`.
- Useful scopes in this repo: `nvmpi`, `ffmpeg`, `ffmpeg-dev`, `ffpatch`, `scripts`, `devcontainer`, `ci`, `docs`.
- Use a `!` after the type/scope (or a `BREAKING CHANGE:` footer) for breaking changes.
- Keep the subject imperative and ≤72 chars; put detail in the body.

Examples: `feat(scripts): add build.sh for libnvmpi`, `fix(nvmpi): guard against null frame buffer`, `docs: document dev-container aliases`.

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

## Attribution policy

Do not include AI attribution in any output. This includes:

- `Co-Authored-By` lines in commit messages
- "Generated with …" lines in MR/PR descriptions, issues, or comments
- Any "AI-generated", "auto-generated", or similar markers in code or documentation

No exceptions.

## Further docs

- `docs/BUILD.md` — full build/install, CMake options, verification.
- `docs/SCRIPTS.md` — every script, command, and dev-container alias.
- `docs/RELEASE.md` — tag-driven release process (GitLab + GitHub releases, per-version archives).
- `docs/DEVELOPMENT.md` — architecture deep-dive, patch system, adding FFmpeg versions, codec registration reference, troubleshooting.
- `docs/DEVCONTAINER.md` — VS Code dev container on Jetson hardware (`.devcontainer/` mounts the host's tegra libs, multimedia API, and CUDA read-only).
