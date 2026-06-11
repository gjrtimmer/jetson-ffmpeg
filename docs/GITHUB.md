# GitHub Actions CI

This project uses GitHub Actions to build the nvmpi library, patch and compile ffmpeg across multiple versions, and run hardware encode/decode tests on NVIDIA Jetson hardware via self-hosted runners.

The GitHub remote is at `https://github.com/gjrtimmer/jetson-ffmpeg.git`.

## Runner Label Convention

| Label | Purpose |
| ----- | ------- |
| _(none / `ubuntu-latest`)_ | Build and patch jobs run on GitHub-hosted runners. No GPU needed. |
| `self-hosted` + `jetson` + `<variant>` | Hardware-specific (e.g. `orin-nx`, `agx`, `nano`). HW test jobs target specific Jetson hardware. |

This means:

- **Build and patch** jobs run on **GitHub-hosted runners** (no GPU needed, stubs build).
- **HW test** jobs run once per variant listed in the CI matrix, targeting self-hosted runners with `jetson` + `<variant>` labels.
- Adding new hardware = request access, register a self-hosted runner with labels `jetson, <variant>`, add a matrix entry.

## Pipeline Overview

> **Status: manual-only.** This workflow is set to `workflow_dispatch`
> (run it from the **Actions** tab) and does **not** trigger on push/PR. It
> needs self-hosted Jetson runners plus arm64 `l4t-jetpack` containers that
> GitHub-hosted runners cannot provide, so on push it only errored. **GitLab CI
> (`.gitlab-ci.yml`) is the active pipeline.** To re-enable push/PR triggers,
> restore the commented-out `push:`/`pull_request:` block in `ci.yml`.

| Job | What it does | Runner |
| --- | ------------ | ------ |
| **build** | Compile nvmpi library with stubs (`WITH_STUBS=ON`) | `ubuntu-latest` |
| **patch** | Clone ffmpeg (matrix: 4.2, 4.4, 6.0, 6.1, 7.0, 7.1, 8.0), apply `scripts/ffpatch.sh`, build | `ubuntu-latest` |
| **hw-test** | Hardware encode/decode smoke test per Jetson variant | `self-hosted, jetson, <variant>` |

## Adding a New Hardware Variant

### Step 1: Request runner access

Open an issue using the **GitHub Runner Token Request** template. A maintainer will add your GitHub account to the repository's runner group.

### Step 2: Set up the runner

Follow the guide matching your environment:

| Environment | Guide |
| ----------- | ----- |
| Kubernetes (k3s / k8s) | [GITHUB_RUNNER_K8S.md](GITHUB_RUNNER_K8S.md) |
| Bare-metal Jetson OS | [GITHUB_RUNNER_MANUAL.md](GITHUB_RUNNER_MANUAL.md) |

### Step 3: Add the variant to the CI matrix

In `.github/workflows/ci.yml`, add your variant to the `hw-test` matrix:

```yaml
hw-test:
  strategy:
    matrix:
      variant:
        - orin-nx
        - agx        # <-- add your variant here
        # - nano
```

Submit a pull request with this change. Once merged, the HW test job runs on your hardware alongside existing variants. If no runner with matching labels is online, that matrix entry stays pending.

## L4T Version

The build/patch jobs run in the `nvcr.io/nvidia/l4t-jetpack:r36.4.0` container. The
tag is hardcoded on the `container.image:` lines in `.github/workflows/ci.yml`
(GitHub Actions does not allow the `env` context in `jobs.<id>.container.image`).
If your Jetson runs a different JetPack/L4T version, edit those two lines:

```yaml
    container:
      image: nvcr.io/nvidia/l4t-jetpack:r36.3.0
```

Check your installed version:

```bash
cat /etc/nv_tegra_release
# or
dpkg -l | grep nvidia-l4t-core
```

## Troubleshooting

### "Waiting for a runner to pick up this job"

- Verify runner is online in **Settings > Actions > Runners**
- Build/patch jobs use `ubuntu-latest` — no self-hosted runner needed
- HW test jobs need a runner with labels `self-hosted`, `jetson`, AND `<variant>`
- If your variant is commented out in the matrix, uncomment it

### Build fails with missing Jetson Multimedia API

The `l4t-jetpack` container image should include it. The workflow installs it as a fallback.

### HW test fails with "nvmpi encoders missing"

- Tegra libraries not available on the runner
- See troubleshooting in [GITHUB_RUNNER_K8S.md](GITHUB_RUNNER_K8S.md#troubleshooting) or [GITHUB_RUNNER_MANUAL.md](GITHUB_RUNNER_MANUAL.md#troubleshooting)

## File Reference

| File | Purpose |
| ---- | ------- |
| `.github/workflows/ci.yml` | GitHub Actions workflow with hardware variant matrix |
| `test/hw-test.sh` | Hardware encode/decode smoke test (runs per variant) |
| `scripts/build.sh` | Build/install libnvmpi (auto-detects stubs vs Jetson) |
| `scripts/ffpatch.sh` | Patches ffmpeg source with nvmpi codec support |
| `ffmpeg/patches/` | Static patch files for older ffmpeg versions |
| `docs/GITHUB_RUNNER_K8S.md` | Runner setup guide for Kubernetes |
| `docs/GITHUB_RUNNER_MANUAL.md` | Runner setup guide for bare-metal Jetson |
| `.github/ISSUE_TEMPLATE/runner-token-request.yml` | Issue template for requesting runner access |
