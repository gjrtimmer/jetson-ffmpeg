# GitHub Runner Setup: Manual (Bare-Metal Jetson)

This guide covers installing a GitHub Actions self-hosted runner directly on a Jetson device without Kubernetes.

## Prerequisites

- Jetson device running JetPack (L4T r36.4.0 or compatible)
- JetPack SDK components installed (Jetson Multimedia API, CUDA, Tegra libraries)
- Docker with NVIDIA runtime (recommended) or run jobs directly on the host
- Runner access granted by a maintainer (see [Requesting Access](#requesting-access))

## 1. Install Docker with NVIDIA Runtime

Docker with the NVIDIA runtime ensures each job gets a clean environment with Tegra library injection.

```bash
sudo apt-get install -y docker.io nvidia-container-toolkit
sudo nvidia-ctk runtime configure --runtime=docker
sudo systemctl restart docker
```

Verify GPU access in Docker:

```bash
sudo docker run --rm --runtime=nvidia nvcr.io/nvidia/l4t-jetpack:r36.4.0 \
  bash -c "ls /usr/lib/aarch64-linux-gnu/tegra/ && echo OK"
```

## 2. Create a Runner User

Create a dedicated user for the runner:

```bash
sudo useradd -m -s /bin/bash github-runner
sudo usermod -aG docker github-runner
sudo usermod -aG video github-runner
```

## 3. Download and Configure the Runner

Get the runner package from **Settings > Actions > Runners > New self-hosted runner** in the GitHub repository. Select **Linux** and **ARM64**.

```bash
sudo -u github-runner bash
cd /home/github-runner

mkdir actions-runner && cd actions-runner
curl -o actions-runner-linux-arm64.tar.gz -L \
  https://github.com/actions/runner/releases/latest/download/actions-runner-linux-arm64-2.325.0.tar.gz
tar xzf actions-runner-linux-arm64.tar.gz
```

> Check the [GitHub Actions Runner releases](https://github.com/actions/runner/releases) for the latest ARM64 version.

## 4. Register the Runner

Register with the repository using the token from GitHub:

```bash
./config.sh \
  --url https://github.com/gjrtimmer/jetson-ffmpeg \
  --token <YOUR_REGISTRATION_TOKEN> \
  --name "jetson-<variant>" \
  --labels "self-hosted,jetson,<variant>" \
  --work "_work" \
  --runasservice
```

Replace `<variant>` with your hardware label (e.g. `orin-nx`, `agx`, `nano`).

> Get the registration token from **Settings > Actions > Runners > New self-hosted runner**.

## 5. Install as a Service

```bash
sudo ./svc.sh install github-runner
sudo ./svc.sh start
```

Verify:

```bash
sudo ./svc.sh status
```

## 6. Configure Environment

The runner needs access to NVIDIA libraries. Create an environment file:

```bash
cat <<'EOF' | sudo tee /home/github-runner/actions-runner/.env
NVIDIA_VISIBLE_DEVICES=all
NVIDIA_DRIVER_CAPABILITIES=all
LD_LIBRARY_PATH=/usr/lib/aarch64-linux-gnu/tegra:/usr/local/cuda/lib64
EOF
```

Restart the service:

```bash
sudo ./svc.sh stop
sudo ./svc.sh start
```

## 7. Verify

Check the runner appears in GitHub:

1. Go to **Settings > Actions > Runners**
2. Runner should show as idle with labels `self-hosted`, `jetson`, and your variant

Quick local test:

```bash
ls /usr/lib/aarch64-linux-gnu/tegra/libnvv4l2.so && echo "Tegra libs OK"
```

## Requesting Access

Self-hosted runners for private repositories must be added by a maintainer:

1. Open an issue using the **Runner Token Request** template
2. A maintainer will provide the registration token or add your account to the runner group
3. Once approved, follow this guide from [step 3](#3-download-and-configure-the-runner)

## Updating the Runner

GitHub releases new runner versions regularly. To update:

```bash
cd /home/github-runner/actions-runner
sudo ./svc.sh stop
curl -o actions-runner-linux-arm64.tar.gz -L \
  https://github.com/actions/runner/releases/latest/download/actions-runner-linux-arm64-2.325.0.tar.gz
tar xzf actions-runner-linux-arm64.tar.gz
sudo ./svc.sh start
```

The runner also auto-updates during job execution when GitHub releases a new version.

## Troubleshooting

### "docker: Error response from daemon: Unknown runtime specified nvidia"

- NVIDIA Container Toolkit is not installed or not configured for Docker
- Run: `sudo nvidia-ctk runtime configure --runtime=docker && sudo systemctl restart docker`

### Runner online but jobs stay pending

- Check runner labels match the job `runs-on` in `.github/workflows/ci.yml`
- HW test jobs need labels: `self-hosted`, `jetson`, AND `<variant>`
- Build/patch jobs use `ubuntu-latest` — no self-hosted runner needed

### "Permission denied" errors

- The `github-runner` user needs access to Docker and NVIDIA devices
- Verify groups: `groups github-runner` should include `docker` and `video`
- Restart the service after group changes

### HW test: "nvmpi encoders missing"

- Tegra libraries not available in the job environment
- Verify `LD_LIBRARY_PATH` includes `/usr/lib/aarch64-linux-gnu/tegra` in the runner's `.env`
- Check `NVIDIA_VISIBLE_DEVICES=all` is set

### Runner goes offline after reboot

- Ensure the service is enabled: `sudo ./svc.sh install github-runner`
- Verify: `systemctl is-enabled actions.runner.*.service`
