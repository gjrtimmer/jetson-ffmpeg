# GitLab Runner Setup: Kubernetes (k3s / k8s)

This guide covers installing a GitLab project runner on a Kubernetes cluster with a Jetson node.

## Prerequisites

- A Jetson device joined to the cluster as a worker node
- JetPack SDK installed on the Jetson (L4T r36.4.0 or compatible)
- `kubectl` access to the cluster
- Helm 3 installed
- A **project runner token** (`glrt-...`) — see [Requesting a Token](#requesting-a-token)

## 1. NVIDIA Container Runtime

The Jetson node must have the NVIDIA container runtime so pods get Tegra library injection.

Verify it is installed:

```bash
nvidia-container-runtime --version
```

If missing:

```bash
sudo apt-get install -y nvidia-container-toolkit
```

### k3s configuration

Create or edit `/var/lib/rancher/k3s/agent/etc/containerd/config.toml.tmpl`:

```toml
[plugins."io.containerd.grpc.v1.cri".containerd]
  default_runtime_name = "nvidia"

[plugins."io.containerd.grpc.v1.cri".containerd.runtimes.nvidia]
  privileged_without_host_devices = false
  runtime_engine = ""
  runtime_root = ""
  runtime_type = "io.containerd.runc.v2"

[plugins."io.containerd.grpc.v1.cri".containerd.runtimes.nvidia.options]
  BinaryName = "/usr/bin/nvidia-container-runtime"
  SystemdCgroup = true
```

Restart k3s:

```bash
sudo systemctl restart k3s
```

### Standard k8s (containerd)

Edit `/etc/containerd/config.toml` and add the NVIDIA runtime, then restart containerd:

```bash
sudo systemctl restart containerd
```

## 2. NVIDIA Device Plugin

Deploy the device plugin DaemonSet so Kubernetes exposes GPU resources:

```bash
kubectl apply -f https://raw.githubusercontent.com/NVIDIA/k8s-device-plugin/v0.17.1/deployments/static/nvidia-device-plugin.yml
```

Verify:

```bash
kubectl describe node <jetson-node> | grep nvidia.com/gpu
```

Expected output: `nvidia.com/gpu: 1`

## 3. RuntimeClass

Create a `RuntimeClass` so the runner can request the NVIDIA runtime:

```yaml
apiVersion: node.k8s.io/v1
kind: RuntimeClass
metadata:
  name: nvidia
handler: nvidia
```

```bash
kubectl apply -f runtimeclass-nvidia.yaml
```

## 4. Install the Runner

Create the namespace and secret with the project runner token:

```bash
kubectl create namespace gitlab-runner 2>/dev/null || true

kubectl create secret generic gitlab-runner-secret \
  --namespace gitlab-runner \
  --from-literal=runner-registration-token="" \
  --from-literal=runner-token="<glrt-YOUR_PROJECT_RUNNER_TOKEN>"
```

Install via Helm:

```bash
helm repo add gitlab https://charts.gitlab.io
helm repo update

helm install gitlab-runner gitlab/gitlab-runner \
  --namespace gitlab-runner \
  --set gitlabUrl=https://gitlab.timmertech.nl/ \
  --set runners.secret=gitlab-runner-secret \
  --set runners.executor=kubernetes
```

> Tags are configured in GitLab when creating the project runner. The runner inherits them from the server.

## 5. Configure for Jetson

Create a `values.yaml` with Jetson-specific settings:

```yaml
runners:
  config: |
    [[runners]]
      [runners.kubernetes]
        namespace = "gitlab-runner"
        image = "nvcr.io/nvidia/l4t-jetpack:r36.4.0"
        runtime_class_name = "nvidia"
        [runners.kubernetes.node_selector]
          kubernetes.io/hostname = "<jetson-node-hostname>"
        [runners.kubernetes.pod_annotations]
          "container.apparmor.security.beta.kubernetes.io/build" = "unconfined"
      [[runners.kubernetes.volumes.empty_dir]]
        name = "tmp"
        mount_path = "/tmp"
    [runners.kubernetes.pod_security_context]
      run_as_non_root = false

  executor: kubernetes
```

Apply:

```bash
helm upgrade gitlab-runner gitlab/gitlab-runner \
  --namespace gitlab-runner \
  -f values.yaml
```

### Key settings

| Setting | Purpose |
| ------- | ------- |
| `runtime_class_name = "nvidia"` | Pods use NVIDIA container runtime; Tegra libs injected automatically |
| `node_selector` | Pins CI pods to the Jetson node |
| `image` | Default build image; should match the L4T version on the device |

## 6. Verify

Check the runner appears in GitLab:

1. Go to **Settings > CI/CD > Runners**
2. Runner should show online with tags `jetson` and your variant

Quick pod test:

```bash
kubectl run gpu-test --rm -it --restart=Never \
  --overrides='{"spec":{"runtimeClassName":"nvidia","nodeSelector":{"kubernetes.io/hostname":"<jetson-node>"}}}' \
  --image=nvcr.io/nvidia/l4t-jetpack:r36.4.0 \
  -- bash -c "ls /usr/lib/aarch64-linux-gnu/tegra/ && echo OK"
```

## Requesting a Token

Project runner tokens are created by maintainers. To request one:

1. Open an issue using the **GitLab Runner Token Request** template
2. A maintainer will create the project runner and share the `glrt-...` token via a secure channel
3. Once you have the token, follow this guide from [step 4](#4-install-the-runner)

## Troubleshooting

### Pod stuck in `ContainerCreating`

- Check `kubectl describe pod <pod>` for events
- Common cause: NVIDIA device plugin not running or RuntimeClass missing

### "nvidia-container-runtime: not found" in pod logs

- The NVIDIA runtime is not configured on the node
- Verify the containerd config and restart k3s/containerd

### Runner online but jobs stay pending

- Check runner tags match the job tags in `.gitlab-ci.yml`
- HW test jobs need both `jetson` AND `<variant>` tags
- Build/patch jobs have no tags — any runner picks them up

### GPU not visible inside pod

- Verify device plugin: `kubectl get pods -n kube-system | grep nvidia`
- Verify node resources: `kubectl describe node <node> | grep nvidia`
- Ensure `NVIDIA_VISIBLE_DEVICES=all` is set (CI config does this for test jobs)
