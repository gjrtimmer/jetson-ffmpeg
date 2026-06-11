# Release Process

Releases are **tag-driven**. Pushing an annotated tag `v<X.Y.Z>` makes the
GitLab pipeline build a real, runnable libnvmpi + FFmpeg for every supported
FFmpeg version, package each as a self-installing archive, publish a **GitLab
Release** with an auto-generated changelog, and mirror it to **GitHub**.

---

## Versioning

The version is the **libnvmpi version** in `CMakeLists.txt`
(`project(nvmpi VERSION X.Y.Z ...)`). The release tag must match it: tag
`v1.1.0` for `VERSION 1.1.0`. Follow [SemVer](https://semver.org/) and
[Conventional Commits](https://www.conventionalcommits.org/) (enforced repo-wide
— see `CLAUDE.md`), which is what the changelog generator keys on.

## One-time setup

1. **CI/CD variables** (Settings → CI/CD → Variables), for the GitHub mirror —
   both **Protected** and (token) **Masked**:
   - `GITHUB_REPO` — `owner/repo`, `owner/repo.git`, or the full
     `https://github.com/owner/repo` URL (all normalized automatically).
   - `GITHUB_TOKEN` — a PAT with `contents:write` on that repo.
   If either is unset, the GitHub mirror step is skipped (GitLab release still runs).
2. **Protected tags** (Settings → Repository → Protected tags): protect `v*`.
   Protected variables are only exposed to protected refs, and releases run on
   tags — without this the mirror won't see `GITHUB_TOKEN`.
3. **Group CI variables** (already provided instance-wide by the cluster
   convention, same as other projects here): `HARBOR_REGISTRY`,
   `DOCKER_HUB_PROXY`, `DOCKER_AUTH_CONFIG`. The release-tools image build uses
   these to reach the buildkit backend and push to Harbor.
4. **Build the release-tools image**: the `build:release-image` job builds
   `ci/release-tools.Dockerfile` (release-cli + git-cliff + gh) via the
   instance's `docker buildx` remote-buildkit backend and pushes it to Harbor as
   `${HARBOR_REGISTRY}/${CI_PROJECT_PATH}/release-tools:latest` (the same
   mechanism as the `docker/arm64` CI component). It runs **automatically** when
   `ci/release-tools.Dockerfile` changes on the default branch, and can be run
   **manually** any time. Build it once before the first release.

## Cutting a release

```bash
# 1. Bump the version and land it on main
#    edit CMakeLists.txt: project(nvmpi VERSION 1.2.0 ...)
git commit -am "chore: release 1.2.0"
git push origin main

# 2. Tag and push (the tag must match the version, prefixed with v)
git tag -a v1.2.0 -m "v1.2.0"
git push origin v1.2.0
```

Pushing the tag starts the release pipeline.

## What the pipeline does on a tag

| Stage | Job(s) | Result |
|-------|--------|--------|
| package | `package:ffmpeg-{4.2,4.4,6.0,6.1,7.0,7.1,8.0}` | On a Jetson runner, build **real** libnvmpi + that FFmpeg (`scripts/build.sh --ffmpeg`), then `scripts/package.sh` → `dist/jetson-ffmpeg_<ver>_ffmpeg<ff>_l4t<tag>_aarch64.tar.gz` |
| release | `release` | `scripts/release.sh`: git-cliff changelog → upload archives to the generic Package Registry → GitLab Release with asset links → mirror to GitHub |

Branch/MR pipelines are unchanged (`build`→`patch`→`test`); they are skipped on
tags, and the release stages only run on tags (`v<X.Y.Z>`). The stub-linked
`build`/`patch` artifacts are not runnable, which is why releases rebuild for
real on hardware.

## The release artifacts

Each archive is self-contained:

```
jetson-ffmpeg_1.2.0_ffmpeg7.1_l4tr36.4.0_aarch64/
├── bin/        ffmpeg, ffprobe (built --enable-nvmpi)
├── lib/        libnvmpi + FFmpeg shared libraries
├── include/
├── install.sh  copies into a prefix, ldconfig, verifies nvmpi codecs
└── README.txt
```

End-user install on a Jetson:

```bash
tar xzf jetson-ffmpeg_1.2.0_ffmpeg7.1_l4tr36.4.0_aarch64.tar.gz
cd jetson-ffmpeg_1.2.0_ffmpeg7.1_l4tr36.4.0_aarch64
sudo ./install.sh            # or: sudo ./install.sh /opt/jetson-ffmpeg
ffmpeg -hide_banner -encoders | grep nvmpi
```

## Local dry-runs

- Build + package one version exactly like CI:
  `scripts/build.sh --ffmpeg 7.1 --install --prefix /tmp/pfx --ffmpeg-prefix /tmp/pfx`
  then `scripts/package.sh --prefix /tmp/pfx --version 1.2.0 --ffmpeg-version 7.1`.
- Full matrix build + hardware test: `test/smoke-all.sh`.
- The changelog/release orchestration (`scripts/release.sh`) needs the GitLab CI
  environment; it is exercised by the actual tag pipeline.

## Files

| File | Role |
|------|------|
| `scripts/package.sh` | Stage a prefix into a versioned archive + `install.sh` |
| `scripts/release.sh` | Changelog + GitLab Release + GitHub mirror (CI) |
| `cliff.toml` | git-cliff changelog config (Conventional Commits) |
| `ci/release-tools.Dockerfile` | Pinned release-cli + git-cliff + gh image |
| `.gitlab-ci.yml` | `package` + `release` stages, `build:release-image` |
