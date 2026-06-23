# Changelog

## 3.6.3 - 2026-06-23

### CI

- Refactor pipeline with `parallel:matrix` builds — 24 individual per-version jobs replaced by 4 matrix definitions (ci)
- Consolidate stages: drop `patch` (fold into `build`), rename `package` to `dist` (ci)
- Rename jobs: `build:nvmpi` → `nvmpi:jp6`/`nvmpi:jp5`, `test:hw-ffmpeg-*` → `test:ffmpeg-*` (ci)
- Simplify release `needs` from 12 entries to 2 matrix parent references (ci)
- Add JetPack 5 build matrix to patch, package, and release stages (ci)
- Dynamically probe optional FFmpeg libs in patch template (ci)
- Add FFmpeg deps to JP5 builder image and make gnutls conditional (ci)
- Rename patch, test, and package jobs with `:jp6` suffix for symmetry (ci)
- Use `if`-form in `probe()` to avoid `set -e` exit on missing packages (ci)
- Replace hardcoded optional FFmpeg flags in dist template with pkg-config probes (ci)

## 3.6.2 - 2026-06-22

### CI

- Upload release assets to S3 instead of GitHub (release)

## 3.6.1 - 2026-06-22

### Bug Fixes

- Guard V4L2 H.264 level 5.2–6.2 and HEVC level 5.2–6.2 constants with `#ifdef` for JetPack 5.x kernel 5.10 compatibility (nvmpi)
- Fix `-Wmisleading-indentation` warning in decoder capture loop (nvmpi)
- Bump CMake minimum version from 3.9 to 3.10 to silence deprecation warning (build)

### CI

- Add JetPack 5.x (L4T r35.4.1) compile-test job and builder image (ci)
- Rename Docker images to distinguish JetPack 5 from JetPack 6 (ci)
- Restrict Docker image builds to `BUILD_IMAGES=true` only — Dockerfile changes no longer auto-trigger builds (ci)

## 3.6.0 - 2026-06-22

### Features

- Add FFmpeg 8.1 support (libavcodec 62, release/8.1 branch) (ffmpeg)

### CI

- Make `--enable-libdav1d` conditional on `dav1d >= 1.0.0` via pkg-config probe (ci)

## 3.5.0 - 2026-06-21

### Bug Fixes

- Correct encoder EOS drain path: distinguish "no packet yet" from "stream fully drained" during flush (ffmpeg)
- Use blocking `get_packet` during encoder flush to prevent CPU spin on EAGAIN (ffmpeg)

## 3.4.0 - 2026-06-21

### Features

- Accept stream dimensions at decoder creation from FFmpeg container headers (decoder)
- Pre-allocate frame pool when dimensions are known, reducing first-frame latency (decoder)
- Skip frame pool rebuild on resolution-change when dimensions match the hint (decoder)
- Wire `avctx->width`/`avctx->height` through FFmpeg decoder wrapper (ffmpeg)

## 3.3.0 - 2026-06-21

### Chores

- Add structured logging macro `NVMPI_LOG` / `NVMPI_LOG_SUB` with compile-time threshold (nvmpi)
- Migrate ~76 logging sites from cerr/cout/fprintf to structured macros (nvmpi)
- Remove all `<iostream>` includes from libnvmpi (nvmpi)

## 3.2.0 - 2026-06-21

### Features

- Add blocking wait to `nvmpi_encoder_get_packet()` mirroring decoder pattern (encoder)
- Activate via `-flags low_delay` in FFmpeg; configurable `wait_timeout` AVOption 0-5000ms (ffmpeg)
- Pool `shutdown()` called before DQ thread stop for clean teardown (encoder)

### Testing

- Add hw-encoder-blocking suite: low_delay H264/HEVC, default path, custom timeout, early termination (encoder)

## 3.1.0 - 2026-06-20

### Features

- Wire `-level` through to V4L2 setLevel for H.264 and H.265 encoders (encoder)
- Add HEVC level mapping using Tegra V4L2_MPEG_VIDEO_H265_LEVEL_*_MAIN_TIER enums (encoder)
- Add missing level constants 5.2/6.0/6.1/6.2 to FFmpeg AVOption table (ffmpeg)

### Testing

- Add hw-encoder-level suite for bitstream-level SPS verification (encoder)

## 3.0.0 - 2026-06-20

### Bug Fixes

- Make TEST_ERROR macro propagate errors instead of ignoring them (nvmpi)

### Documentation

- Update supported FFmpeg version range to 6.0+

### Refactor

- Drop FFmpeg 4.2 and 4.4 support (ffmpeg)
## 2.10.0 - 2026-06-19

### Bug Fixes

- Resolve decoder capture-loop race on FFmpeg 7.0+ threaded model (nvmpi)
- Add goto-cleanup error handling in encoder init (nvmpi)

### Build

- Add Tegra libjpeg HW accel stubs for NvJpegEncoder (stubs)

### Chores

- 0668aaa3 session findings, skill v15→v16 (retro)
- 2d112242 session findings, skill v16 (no skill change) (retro)
- Release 2.10.0

### Features

- Hardware MJPEG encoder via NvJPEGEncoder::encodeFromFd (nvmpi)

### Testing

- Add MJPEG encoder hardware test suite (nvmpi)
## 2.9.0 - 2026-06-19

### Bug Fixes

- Retry V4L2 device creation on transient failure (nvmpi)

### Build

- Add NvJpegDecoder to build and stub for off-Jetson CI (stubs)
- Link libjpeg for NvJpegDecoder standard jpeg symbols

### Chores

- Release 2.9.0

### Features

- Add hardware MJPEG decoder via NvJPEGDecoder (nvmpi)

### Testing

- Add MJPEG hardware decoder test suite (decoder)
## 2.8.2 - 2026-06-18

### Bug Fixes

- Reduce decoder capture thread CPU overhead (nvmpi)
- Use atomics for cross-thread flags, unique_ptr for NvVideoEncoder (encoder)

### Chores

- Session d4b84d2a findings (retro)
- Trigger MR pipeline
- D4b84d2a session findings, skill v14→v15 (retro)
- Release 2.8.2

### Testing

- Add hw-soak-decode suite for CPU regression guard
- Add concurrent multi-stream encode stress suite (encoder)
## 2.8.1 - 2026-06-17

### Bug Fixes

- Guard initFramePool against invalid pool size and double-init (decoder)
- Per-suite timeout in hw-all.sh and idempotent release creation (ci)

### Chores

- Exempt release commits from smoke-all gate
- Release 2.8.1

### Testing

- Fix CI collapsed section timing and hw-stats message
## 2.8.0 - 2026-06-17

### Bug Fixes

- Suppress NVIDIA stdout in latency measurement functions (test)
- Replace flawed latency comparison with completion check (test)
- Suppress unused variable warnings in test_bufpool (test)
- Lower decoder-flush seek-to-4s threshold from 10 to 5 frames (test)
- Guard nvmpi_create_decoder against NULL deref on V4L2 failure (nvmpi)

### Build

- Gitignore .work/ and untrack design docs
- Regenerate patches for decoder NULL-deref fix (ffmpeg)

### CI

- Point runner-token templates and pipeline comments to the wiki

### Chores

- Update issue #10 status tracking file
- Regenerate patches for all FFmpeg versions (ffmpeg-dev)
- Update issue #10 status tracking file
- Add VS Code C/C++ IntelliSense configuration (devcontainer)
- Add domain-specific words to cSpell dictionary (devcontainer)
- Remove TODO.md — all items tracked as GitHub issues
- Iteration 1 — sessions 1-5 findings, skill v1→v2 (retro)
- Iteration 2 — sessions 6-10 findings, skill v2→v3 (retro)
- Iteration 3 — sessions 11-14 findings, skill v3→v4 (retro)
- Iteration 4 — sessions 15-20 findings, skill v4→v5 (retro)
- Iteration 5 — memory audit + stale cleanup, skill v5→v6 (retro)
- Session a79c607d findings, skill v9→v10 (retro)
- Session findings, skill v10→v11 (retro)
- Add mandatory issue labelling rule (retro)
- Session 4e1f4f38 findings, skill v11→v12 (retro)
- A79c session findings, skill v12→v13 (retro)
- Session d1622f65 findings, skill v13→v14 (retro)
- Release 2.8.0

### Documentation

- Add ARCHITECTURE.md with modular split convention
- Update DEVELOPMENT.md with modular file structure
- Add issue #10 design spec and implementation plan
- Add THREAD_SAFETY.md, API_REFERENCE.md, update BUILD/README/TODO (nvmpi)
- Add Fixes #N pre-push gate to CLAUDE.md issue closing rules
- Update ARCHITECTURE.md, DEVELOPMENT.md for encoder modular split
- Update references for NVMPI_frameBuf → nvmpi_frame_buffer rename
- Update README with encoder and API test suites (test)
- Add ci.skip rule for branch push before MR creation
- Fold session findings into CLAUDE.md, retro skill v6→v7
- Second-pass findings + pre-push retro gate, skill v7→v8
- Scope to current-session by default, idempotent re-runs, v8→v9 (retro)
- Add vcs-cli gh+glab command cheatsheet (skills)
- Point documentation links to the wiki (readme)
- Retarget docs refs to wiki + add standing wiki-docs rule
- Add GitHub wiki commands to vcs-cli agent; retarget doc refs (skills)
- Migrate BUILD.md to wiki (Build and Install)
- Migrate COMPATIBILITY.md to wiki (Compatibility)
- Migrate SCRIPTS.md to wiki (Scripts and Commands)
- Migrate DEVELOPMENT.md to wiki (Development Guide)
- Migrate ARCHITECTURE.md to wiki (Architecture)
- Migrate API_REFERENCE.md to wiki (API Reference)
- Migrate THREAD_SAFETY.md to wiki (Thread Safety)
- Migrate DEVCONTAINER.md to wiki (Dev Container)
- Migrate GITHUB.md to wiki (GitHub Actions)
- Migrate GITLAB.md to wiki (GitLab CI)
- Migrate GITHUB_RUNNER_MANUAL.md to wiki (GitHub Runner Manual)
- Migrate GITHUB_RUNNER_K8S.md to wiki (GitHub Runner Kubernetes)
- Migrate GITLAB_RUNNER_MANUAL.md to wiki (GitLab Runner Manual)
- Migrate GITLAB_RUNNER_K8S.md to wiki (GitLab Runner Kubernetes)
- Migrate RELEASE.md to wiki (Release Process)
- Migrate FORKS.md to wiki (Fork Network Overview)
- Migrate FORKS_REPORT.md to wiki (Fork Analysis Report)
- Migrate FORKS_RESULT.md to wiki (Fork Analysis Results)
- Add FAQ section to wiki
- Retarget code-comment doc refs to wiki; regenerate patches (nvmpi)

### Features

- Add condition-variable blocking dequeue to NVMPI_bufPool (nvmpi)
- Make ctx->eos atomic and add wait_timeout_ms (nvmpi)
- Wire shutdown() to capture-loop exit and use atomic eos (nvmpi)
- Implement blocking wait in nvmpi_decoder_get_frame() (nvmpi)
- Add wait_timeout AVOption for low-delay decoder (ffmpeg)
- Blocking wait in decoder get_frame (nvmpi)
- Add session retrospective skill v1 (retro)

### Refactor

- Extract nvmpictx struct to nvmpi_dec_internal.h (nvmpi)
- Extract capture loop to nvmpi_dec_capture.cpp (nvmpi)
- Fix copyNvBufToFrame forward declaration signature (nvmpi)
- Extract V4L2 plane setup to nvmpi_dec_planes.cpp (nvmpi)
- Rename nvmpi_dec.cpp to nvmpi_dec_api.cpp (nvmpi)
- Extract nvmpi_enc_internal.h from nvmpi_enc.cpp (nvmpi)
- Extract nvmpi_enc_output.cpp from nvmpi_enc.cpp (nvmpi)
- Rename nvmpi_enc.cpp → nvmpi_enc_api.cpp; update CMakeLists (nvmpi)
- Modular split of encoder source files (nvmpi)
- Rename NVMPI_frameBuf to nvmpi_frame_buffer (nvmpi)
- Convert vcs-cli skill to a sonnet agent (skills)

### Styling

- Fix markdown lint errors across all documentation (docs)

### Testing

- Add unit tests for NVMPI_bufPool blocking dequeue (nvmpi)
- Add hw test suites for blocking wait, lifecycle, and perf (nvmpi)
- Add pool-size boundary and lifecycle stress suites (encoder)
- Add libnvmpi C API smoke test harness (api)
- Add V4L2 recovery delay before post-stress health check (decoder)
## 2.7.0 - 2026-06-15

### Bug Fixes

- Install jetson-stats as root (devcontainer)
- Relax insert_vui assertion for FFmpeg 6.1+ tick rate (test)

### Build

- Auto-enable libx265, add to devcontainer

### CI

- Add jetson-status to jetpack image for testing stage

### Chores

- Release 2.7.0

### Documentation

- Update coding rules
- Add commit-splitting rules to CLAUDE.md

### Features

- Pixel formats — YUVJ420P, NV12 I/O, P010, insert_vui (nvmpi,ffmpeg)

### Testing

- Hw-format-pixfmt suite for pixel format features (ffmpeg)
## 2.6.0 - 2026-06-14

### Bug Fixes

- Remove unsafe ff_get_buffer call from flush callback (ffmpeg)

### CI

- Add l4t-jetpack build stage
- Restrict l4t-jetpack build to scheduled pipelines
- Push l4t-jetpack image to DockerHub
- Add build-images.sh for local CI image builds (scripts)
- Add release-tools to build-images.sh, push all to Harbor + DockerHub (scripts)

### Chores

- Fix verification phases in fix-issue skill (skills)
- Release 2.6.0

### Features

- Implement decoder flush for seek / stream restart (nvmpi)
## 2.5.0 - 2026-06-14

### Documentation

- Add auto-merge rule to CLAUDE.md and fix-issue skill

### Performance

- Enable max-perf mode, optional DPB disable and poc-type (nvmpi)
## 2.4.0 - 2026-06-13

### Bug Fixes

- Close GitHub issue for glibc>=2.34 pthread_join segfault (nvmpi)
- Fix decoder crash on close — 7 teardown bugs (nvmpi)

### Documentation

- Add fix-issue skill and branch naming convention
- Add issue status comments to fix-issue skill (skills)
- Enhance fix-issue skill — activation, MR, fork notifications (skills)
## 2.3.0 - 2026-06-13

### Bug Fixes

- Relax decoder-codecs frame threshold to 80% (test)
- Restrict image builds to scheduled pipeline only (ci)
- Set docker CLI image on component jobs, always pull builder (ci)
- Guard pthread_join in NvV4l2ElementPlane for glibc>=2.34 (build)

### CI

- Run test/hw-all.sh in hardware test jobs
- Deduplicate branch/MR pipelines via workflow rules
- Add builder image, move CI images to DockerHub

### Documentation

- Add empty-commit rule for metadata-only changes
- Add upstream notification rule
- Add issue closing rules
- Persist session working rules in CLAUDE.md

### Performance

- Use lighter l4t-jetpack image for hw-test jobs (ci)

### Refactor

- Use docker component for image builds (ci)

### Testing

- Split hw-test.sh into per-feature suites with hw-all.sh runner
- Add decoder coverage suites (codecs, downscale, pool)
## 2.2.0 - 2026-06-12

### Bug Fixes

- Order of install due to permissions (devcontainer)
- Bashrc (devcontainer)
- Update (scripts)
- Update (scripts)
- Move uv/uvx from devcontainer to user space (scripts)
- Skills install (scripts)
- Upstream config (caveman)
- Add --yes flag to brew install and include jq (devcontainer)
- Bound oversized packets and add configurable chunk_size (nvmpi)
- Never return EAGAIN from the decode callback (ffmpeg-dev)
- Reference decode-callback EAGAIN abort issue (ffmpeg-dev)

### Build

- Pull l4t-jetpack image from harbor.local/jetson registry

### CI

- Move package stage off Jetson runners (package)

### Chores

- Add tegra/CI domain words to cSpell dictionary
- Add mcp servers / plugins / skills script
- Add skills / plugins

### Documentation

- Require glab ci lint after .gitlab-ci.yml edits
- Drop nonexistent l4t-base:r36.4.0 from runtime checks (devcontainer)
- Consolidate JPEG analysis into FORKS_RESULT.md
- Add attribution policy to CLAUDE.md
- Comprehensive fork network analysis of 237 forks (forks)
- Update
- Add GitHub issue tracker repo to CLAUDE.md
- Correct stale our-repo status rows in FORKS_RESULT (forks)

### Features

- Add nvm /node / uv / uvx (devcontainer)
- Add shellcheck (devcontainer)
- Pass envvars (devcontainer)
## 2.1.0 - 2026-06-12

### Bug Fixes

- Prime decoder with out-of-band extradata at init (ffmpeg-dev)

### Build

- Make libnvmpi build warning-free under -Wall -Wextra (nvmpi)

### Chores

- Set Buy Me a Coffee username to 'gjrtimmer'
- Close issue #1 - out-of-band extradata priming fix landed

### Documentation

- Document RTP out-of-band decode tests in hw-test
- Move hardware support tables to COMPATIBILITY.md, rewrite intro

### Testing

- Add RTP decode cases for out-of-band parameter sets (hw-test)
## 2.0.0 - 2026-06-11

### Bug Fixes

- Print diagnostic messages to stderr instead of stdout
- Redirect remaining stdout messages to stderr
- Bundle libnvmpi in ffmpeg artifacts for hw test
- Mount host tegra libs, CUDA, and multimedia API (devcontainer)
- Remove redundant twxs.cmake extension (vscode)
- Remove postCreateCommand (devcontainer)
- Use .bash_aliases for PS1 prompt (devcontainer)
- Fix PS1 prompt sourcing (devcontainer)
- Fix PS1 by appending to .bashrc (devcontainer)
- Use PROMPT_COMMAND for PS1 (devcontainer)
- Build .bashrc as root before USER switch (devcontainer)
- Set bash as default shell (devcontainer)
- Always pass explicit CMAKE_INSTALL_PREFIX in build.sh (scripts)
- Install libx264-dev and drop stale branch step (devcontainer)
- Pass --prefix= (equals form) to ffmpeg configure (scripts)

### CI

- Add GitLab CI pipeline and runner documentation
- Add GitHub Actions pipeline and runner documentation
- Point ffpatch invocation at scripts/ffpatch.sh
- Add ffmpeg 8.0 to GitHub and GitLab patch matrices
- Disable GitHub Actions auto-triggers and fix invalid env usage
- Hw-test the breakpoint FFmpeg versions per wrapper path (gitlab)
- Hw-test all seven FFmpeg versions (gitlab)
- Build release-tools image via buildkit->Harbor (drop kaniko) (release)
- Serialize Jetson jobs with resource_group to avoid overload

### Chores

- Add vscode settings
- Add issue templates for GitHub and GitLab
- Remove runner token request template
- Remove runner token request template
- Add copyright for G.J.R. Timmer (license)
- Suppress InvalidBaseImagePlatform warning (devcontainer)
- Add cSpell words for project terminology (vscode)
- Add claude
- Add .gitattributes to enforce LF line endings
- Normalize files to LF
- 2.0.0 (release)

### Documentation

- Add development guide, build guide, and slim down README
- Add repo clone step and SSH platform config (devcontainer)
- Update devcontainer
- Update paths, add layout, scripts and commit conventions (claude)
- Update paths and reference build.sh across guides
- Add SCRIPTS reference and document dev-container aliases
- Bring CLAUDE.md and CI docs up to date
- Add extensive explanatory comments to libnvmpi and ffmpeg wrappers (src)
- Use glab/gh CLIs for GitLab/GitHub and require auth check (claude)

### Features

- Name decoder capture thread for easier debugging
- Add devcontainer for Jetson development over remote SSH
- Add development extensions (devcontainer)
- Configure workspace extension recommendations (vscode)
- Add development tools and FFmpeg build deps (devcontainer)
- Add claude-code and cpptools-extension-pack (vscode)
- Add claude-code, docker, and cpptools-extension-pack (devcontainer)
- Add PS1 prompt with hostname and git branch (devcontainer)
- Add Homebrew with gh and glab CLI tools (devcontainer)
- Add editor settings and editorconfig (vscode)
- Add Claude Code CLI via Homebrew (devcontainer)
- Persist home directory with named volume (devcontainer)
- Add runner token request issue templates (github)
- Add runner token request issue template for GitHub (gitlab)
- Add build.sh for libnvmpi (scripts)
- Add project command aliases (devcontainer)
- Add --ffmpeg option to build.sh (scripts)
- Ship committed patches for all 7 versions, regenerated (ffmpeg)
- Add packaging script and bump nvmpi to 1.1.0 (release)
- Tag-driven GitLab+GitHub releases with per-version archives (release)
- Self-contained archives (apt-or-bundle deps) + harden cleanup rm (release)
- Install.sh auto-elevates with sudo (release)

### Refactor

- Move ffmpeg_dev and ffmpeg_patches under ffmpeg/ (ffmpeg)
- Move ffpatch.sh into scripts/ (scripts)
- Resolve paths from script location (ffpatch)
- Make patch scripts path-independent (ffmpeg-dev)

### Testing

- Add nvmpi hardware encode/decode smoke test
- Add full cross-version smoke-all harness
