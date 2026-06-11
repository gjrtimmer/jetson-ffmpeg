# GitLab CI

This project uses GitLab CI to build the nvmpi library, patch and compile ffmpeg across multiple versions, and run hardware encode/decode tests on NVIDIA Jetson hardware.

The canonical GitLab remote is `origin` at `https://gitlab.timmertech.nl/timmertech/jetson-ffmpeg.git`.

## Runner Tag Convention

| Tag | Purpose |
| --- | ------- |
| _(none)_ | Build and patch stages have **no tags** — they run on any available runner. |
| `jetson` + `<variant>` | Hardware-specific (e.g. `orin-nx`, `agx`, `nano`). HW test jobs target specific Jetson hardware. |

This means:

- **Build and patch** jobs run on **any available runner** (no GPU needed, stubs build).
- **HW test** jobs run once per variant listed in the CI matrix, targeting `jetson` + `<variant>`.
- Adding new hardware = request a token, register a project runner with `jetson, <variant>`, add a matrix entry.

## Pipeline Overview

| Stage | Job(s) | What it does | GPU required |
| ----- | ------ | ------------ | :----------: |
| **build** | `build:nvmpi` | Compile the nvmpi library with stubs (`WITH_STUBS=ON`) | No |
| **patch** | `patch:ffmpeg-{4.2,4.4,6.0,6.1,7.0,7.1}` | Clone ffmpeg, apply `ffpatch.sh`, configure and build ffmpeg with nvmpi | No |
| **test** | `test:hw-encode: [<variant>]` | Hardware encode/decode smoke test per Jetson variant | **Yes** |

## Adding a New Hardware Variant

### Step 1: Request a runner token

Open an issue using the **GitLab Runner Token Request** template. A maintainer will create a project runner with tags `jetson, <variant>` and share the `glrt-...` token with you.

### Step 2: Set up the runner

Follow the guide matching your environment:

| Environment | Guide |
| ----------- | ----- |
| Kubernetes (k3s / k8s) | [GITLAB_RUNNER_K8S.md](GITLAB_RUNNER_K8S.md) |
| Bare-metal Jetson OS | [GITLAB_RUNNER_MANUAL.md](GITLAB_RUNNER_MANUAL.md) |

### Step 3: Add the variant to the CI matrix

In `.gitlab-ci.yml`, add your variant to the `test:hw-encode` parallel matrix:

```yaml
test:hw-encode:
  parallel:
    matrix:
      - JETSON_VARIANT: orin-nx
      - JETSON_VARIANT: agx        # <-- add your variant here
      # - JETSON_VARIANT: nano
```

Submit a merge request with this change. Once merged, the HW test job runs on your hardware alongside existing variants. If no runner with matching tags is available, that matrix entry stays pending until one comes online.

## L4T Version

The pipeline uses `L4T_TAG: "r36.4.0"` by default. If your Jetson runs a different JetPack/L4T version, update the variable in `.gitlab-ci.yml`:

```yaml
variables:
  L4T_TAG: "r36.3.0"  # match your JetPack version
```

Check your installed version:

```bash
cat /etc/nv_tegra_release
# or
dpkg -l | grep nvidia-l4t-core
```

## Troubleshooting

### "No matching runner found"

- Verify runner is online in **Settings > CI/CD > Runners**
- Build/patch jobs have no tags — any runner can pick them up
- HW test jobs need a runner tagged both `jetson` AND `<variant>` matching the matrix entry
- If your variant is commented out in the matrix, uncomment it

### Build fails with missing Jetson Multimedia API

The `l4t-jetpack` container image should include it. If not:

```bash
apt-get install -y nvidia-l4t-jetson-multimedia-api
```

The CI `before_script` already handles this fallback.

### HW test fails with "nvmpi encoders missing"

- The NVIDIA runtime is not injecting Tegra libraries
- See troubleshooting in [GITLAB_RUNNER_K8S.md](GITLAB_RUNNER_K8S.md#troubleshooting) or [GITLAB_RUNNER_MANUAL.md](GITLAB_RUNNER_MANUAL.md#troubleshooting)

### Artifacts too large or jobs timing out

- Reduce the ffmpeg version matrix (remove older versions from `.gitlab-ci.yml`)
- Increase the runner pod/host resource limits
- Use a persistent volume or cache for build artifacts

## File Reference

| File | Purpose |
| ---- | ------- |
| `.gitlab-ci.yml` | Pipeline definition with hardware variant matrix |
| `test/hw-test.sh` | Hardware encode/decode smoke test (runs per variant) |
| `ffpatch.sh` | Patches ffmpeg source with nvmpi codec support |
| `ffmpeg_patches/` | Static patch files for older ffmpeg versions |
| `docs/GITLAB_RUNNER_K8S.md` | Runner setup guide for Kubernetes |
| `docs/GITLAB_RUNNER_MANUAL.md` | Runner setup guide for bare-metal Jetson |
| `.gitlab/issue_templates/GitLab Runner Token Request.md` | Issue template for requesting a runner token |
