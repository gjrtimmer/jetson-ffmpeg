# Changelog

## 2.6.0 - 2026-06-14

### Features

- Implement decoder flush for seek / stream restart (nvmpi)

### Bug Fixes

- Remove unsafe ff_get_buffer call from flush callback (ffmpeg)

### CI

- Add l4t-jetpack build stage
- Restrict l4t-jetpack build to scheduled pipelines
- Push l4t-jetpack image to DockerHub
- Add build-images.sh for local CI image builds (scripts)
- Add release-tools to build-images.sh, push all to Harbor + DockerHub (scripts)

## 2.5.0 - 2026-06-14

### Performance

- Enable max-perf mode, optional DPB disable and poc-type (nvmpi)

## 2.4.0 - 2026-06-13

### Bug Fixes

- Close GitHub issue for glibc>=2.34 pthread_join segfault (nvmpi)
- Fix decoder crash on close — 7 teardown bugs (nvmpi)

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

### Performance

- Use lighter l4t-jetpack image for hw-test jobs (ci)

### Refactor

- Use docker component for image builds (ci)

### Testing

- Split hw-test.sh into per-feature suites with hw-all.sh runner
- Add decoder coverage suites (codecs, downscale, pool)

## 2.2.0 - 2026-06-12

### Features

- Add nvm /node / uv / uvx (devcontainer)
- Add shellcheck (devcontainer)
- Pass envvars (devcontainer)

### Bug Fixes

- Bound oversized packets and add configurable chunk_size (nvmpi)
- Never return EAGAIN from the decode callback (ffmpeg-dev)
- Reference decode-callback EAGAIN abort issue (ffmpeg-dev)
- Order of install due to permissions (devcontainer)
- Add --yes flag to brew install and include jq (devcontainer)

### Build

- Pull l4t-jetpack image from harbor.local/jetson registry

### CI

- Move package stage off Jetson runners (package)

## 2.1.0 - 2026-06-12

### Features

- Decoder: fix zero-frame decode of RTSP/RTP streams whose SPS/PPS arrive out-of-band

### Bug Fixes

- Prime decoder with out-of-band extradata at init (ffmpeg-dev)

### Build

- Make libnvmpi build warning-free under -Wall -Wextra (nvmpi)

### Testing

- Add RTP decode cases for out-of-band parameter sets (hw-test)

## 2.0.0 - 2026-06-11

### Features

- Name decoder capture thread for easier debugging
- Add devcontainer for Jetson development over remote SSH
- Add build.sh for libnvmpi (scripts)
- Add --ffmpeg option to build.sh (scripts)
- Ship committed patches for all 7 versions, regenerated (ffmpeg)
- Add packaging script and bump nvmpi to 1.1.0 (release)
- Tag-driven GitLab+GitHub releases with per-version archives (release)
- Self-contained archives (apt-or-bundle deps) + harden cleanup rm (release)
- Install.sh auto-elevates with sudo (release)

### Bug Fixes

- Print diagnostic messages to stderr instead of stdout
- Redirect remaining stdout messages to stderr
- Bundle libnvmpi in ffmpeg artifacts for hw test
- Always pass explicit CMAKE_INSTALL_PREFIX in build.sh (scripts)
- Pass --prefix= (equals form) to ffmpeg configure (scripts)

### CI

- Add GitLab CI pipeline and runner documentation
- Add GitHub Actions pipeline and runner documentation
- Add ffmpeg 8.0 to GitHub and GitLab patch matrices
- Hw-test the breakpoint FFmpeg versions per wrapper path (gitlab)
- Hw-test all seven FFmpeg versions (gitlab)
- Build release-tools image via buildkit->Harbor (drop kaniko) (release)
- Serialize Jetson jobs with resource_group to avoid overload

### Refactor

- Move ffmpeg_dev and ffmpeg_patches under ffmpeg/ (ffmpeg)
- Move ffpatch.sh into scripts/ (scripts)
- Resolve paths from script location (ffpatch)
- Make patch scripts path-independent (ffmpeg-dev)

### Testing

- Add nvmpi hardware encode/decode smoke test
- Add full cross-version smoke-all harness
