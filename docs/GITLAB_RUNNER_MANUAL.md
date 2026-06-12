# GitLab Runner Setup: Manual (Bare-Metal Jetson)

This guide covers installing a GitLab project runner directly on a Jetson device without Kubernetes.

## Prerequisites

- Jetson device running JetPack (L4T r36.4.0 or compatible)
- JetPack SDK components installed (Jetson Multimedia API, CUDA, Tegra libraries)
- Docker with NVIDIA runtime **or** the runner executes jobs in `shell` mode
- A **project runner token** (`glrt-...`) — see [Requesting a Token](#requesting-a-token)

## 1. Install GitLab Runner

Add the GitLab Runner repository and install for `arm64`:

```bash
curl -L "https://packages.gitlab.com/install/repositories/runner/gitlab-runner/script.deb.sh" | sudo bash
sudo apt-get install -y gitlab-runner
```

Verify:

```bash
gitlab-runner --version
```

## 2. Install Docker with NVIDIA Runtime (Docker executor)

Skip this section if you plan to use the `shell` executor.

```bash
sudo apt-get install -y docker.io nvidia-container-toolkit
sudo nvidia-ctk runtime configure --runtime=docker
sudo systemctl restart docker
```

Verify GPU access in Docker:

```bash
sudo docker run --rm --runtime=nvidia harbor.local/jetson/l4t-jetpack:r36.4.0 \
  bash -c "ls /usr/lib/aarch64-linux-gnu/tegra/ && echo OK"
```

## 3. Register the Runner

### Docker executor (recommended)

The Docker executor runs each job in a fresh container. NVIDIA runtime injects Tegra libraries automatically.

```bash
sudo gitlab-runner register \
  --non-interactive \
  --url "https://gitlab.timmertech.nl/" \
  --token "<glrt-YOUR_PROJECT_RUNNER_TOKEN>" \
  --executor "docker" \
  --docker-image "harbor.local/jetson/l4t-jetpack:r36.4.0" \
  --docker-runtime "nvidia" \
  --description "jetson-<variant>" \
  --docker-volumes "/tmp:/tmp"
```

### Shell executor

The shell executor runs jobs directly on the host. All dependencies must be pre-installed on the system.

```bash
sudo gitlab-runner register \
  --non-interactive \
  --url "https://gitlab.timmertech.nl/" \
  --token "<glrt-YOUR_PROJECT_RUNNER_TOKEN>" \
  --executor "shell" \
  --description "jetson-<variant>"
```

Required system packages for the shell executor:

```bash
sudo apt-get install -y \
  git build-essential cmake pkg-config yasm nasm wget ca-certificates \
  libv4l-dev libx264-dev libx265-dev libnuma-dev libvpx-dev libopus-dev \
  libmp3lame-dev libvorbis-dev libdav1d-dev libass-dev libfreetype-dev \
  libgnutls28-dev
```

> Tags are configured in GitLab when creating the project runner. The runner inherits them from the server.

## 4. Configure the Runner

Edit `/etc/gitlab-runner/config.toml` to fine-tune settings.

### Docker executor config

```toml
[[runners]]
  name = "jetson-<variant>"
  url = "https://gitlab.timmertech.nl/"
  executor = "docker"
  [runners.docker]
    image = "harbor.local/jetson/l4t-jetpack:r36.4.0"
    runtime = "nvidia"
    privileged = false
    volumes = ["/tmp:/tmp", "/cache"]
    shm_size = 0
  [runners.docker.tmpfs]
    "/tmp" = "rw,noexec"
```

### Shell executor config

```toml
[[runners]]
  name = "jetson-<variant>"
  url = "https://gitlab.timmertech.nl/"
  executor = "shell"
  environment = [
    "NVIDIA_VISIBLE_DEVICES=all",
    "NVIDIA_DRIVER_CAPABILITIES=all"
  ]
```

Restart after changes:

```bash
sudo gitlab-runner restart
```

## 5. Enable as a Service

Ensure the runner starts on boot:

```bash
sudo systemctl enable gitlab-runner
sudo systemctl start gitlab-runner
```

Check status:

```bash
sudo gitlab-runner status
sudo gitlab-runner verify
```

## 6. Verify

Check the runner appears in GitLab:

1. Go to **Settings > CI/CD > Runners**
2. Runner should show online with tags `jetson` and your variant

Run a quick local test:

```bash
# Verify GPU/Tegra access
ls /usr/lib/aarch64-linux-gnu/tegra/libnvv4l2.so && echo "Tegra libs OK"
```

## Requesting a Token

Project runner tokens are created by maintainers. To request one:

1. Open an issue using the **GitLab Runner Token Request** template
2. A maintainer will create the project runner and share the `glrt-...` token via a secure channel
3. Once you have the token, follow this guide from [step 3](#3-register-the-runner)

## Troubleshooting

### "docker: Error response from daemon: Unknown runtime specified nvidia"

- NVIDIA Container Toolkit is not installed or not configured for Docker
- Run: `sudo nvidia-ctk runtime configure --runtime=docker && sudo systemctl restart docker`

### Runner online but jobs stay pending

- Check runner tags match the job tags in `.gitlab-ci.yml`
- HW test jobs need both `jetson` AND `<variant>` tags
- Build/patch jobs have no tags — any runner picks them up

### Shell executor: "command not found" during build

- All build dependencies must be installed on the host system
- Check the package list in [step 3](#3-register-the-runner) under shell executor

### HW test: "nvmpi encoders missing"

- Tegra libraries not available in the job environment
- Docker executor: ensure `runtime = "nvidia"` in runner config
- Shell executor: ensure `NVIDIA_VISIBLE_DEVICES=all` in environment and Tegra libs are on `LD_LIBRARY_PATH`

### Permission denied errors (shell executor)

- The `gitlab-runner` user needs access to NVIDIA devices
- Add to the video group: `sudo usermod -aG video gitlab-runner`
- Restart: `sudo gitlab-runner restart`
