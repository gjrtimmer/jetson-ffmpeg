# Changelog

## 2.7.0 - 2026-06-15

### Bug Fixes

- Install jetson-stats as root (devcontainer)
- Relax insert_vui assertion for FFmpeg 6.1+ tick rate (test)

### Build

- Auto-enable libx265, add to devcontainer

### CI

- Add jetson-status to jetpack image for testing stage

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
