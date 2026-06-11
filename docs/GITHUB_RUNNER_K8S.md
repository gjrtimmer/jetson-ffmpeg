# GitHub Runner Setup: Kubernetes (k3s / k8s)

This guide covers installing a GitHub Actions self-hosted runner on a Kubernetes cluster with a Jetson node.

## Prerequisites

- A Jetson device joined to the cluster as a worker node
- JetPack SDK installed on the Jetson (L4T r36.4.0 or compatible)
- `kubectl` access to the cluster
- Helm 3 installed
- Runner access granted by a maintainer (see [Requesting Access](#requesting-access))

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

Create a `RuntimeClass` so runner pods can request the NVIDIA runtime:

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

## 4. Install Actions Runner Controller (ARC)

GitHub's recommended approach for Kubernetes is [Actions Runner Controller](https://github.com/actions/actions-runner-controller).

### Install ARC via Helm

```bash
helm repo add actions-runner-controller https://actions-runner-controller.github.io/actions-runner-controller
helm repo update

helm install arc actions-runner-controller/actions-runner-controller \
  --namespace actions-runner-system --create-namespace \
  --set authSecret.create=true \
  --set authSecret.github_token="<YOUR_GITHUB_PAT>"
```

> The PAT needs `repo` scope for private repositories or `admin:org` for organization runners.

### Create a RunnerDeployment

Create a `runner-deployment.yaml` targeting this repository:

```yaml
apiVersion: actions.summerwind.dev/v1alpha1
kind: RunnerDeployment
metadata:
  name: jetson-runner
  namespace: actions-runner-system
spec:
  replicas: 1
  template:
    spec:
      repository: gjrtimmer/jetson-ffmpeg
      labels:
        - jetson
        - <variant>
      nodeSelector:
        kubernetes.io/hostname: "<jetson-node-hostname>"
      runtimeClassName: nvidia
      env:
        - name: NVIDIA_VISIBLE_DEVICES
          value: "all"
        - name: NVIDIA_DRIVER_CAPABILITIES
          value: "all"
      containers:
        - name: runner
          image: summerwind/actions-runner:latest
          resources:
            limits:
              nvidia.com/gpu: "1"
```

Replace `<variant>` with your hardware label (e.g. `orin-nx`, `agx`, `nano`).

```bash
kubectl apply -f runner-deployment.yaml
```

### Alternative: GitHub-hosted runner registration token

If you prefer not to use ARC, you can run the standard GitHub Actions runner in a pod. Get a registration token from **Settings > Actions > Runners > New self-hosted runner** and follow the manual approach inside the pod.

## 5. Verify

Check the runner appears in GitHub:

1. Go to **Settings > Actions > Runners**
2. Runner should show as idle with labels `self-hosted`, `jetson`, and your variant

Verify GPU access in the runner pod:

```bash
kubectl exec -it -n actions-runner-system <runner-pod> -- \
  bash -c "ls /usr/lib/aarch64-linux-gnu/tegra/ && echo OK"
```

## Requesting Access

Self-hosted runners for private repositories must be added by a maintainer:

1. Open an issue using the **GitHub Runner Token Request** template
2. A maintainer will add your account to the repository's runner group
3. Once approved, follow this guide from [step 4](#4-install-actions-runner-controller-arc)

## Troubleshooting

### Pod stuck in `ContainerCreating`

- Check `kubectl describe pod <pod>` for events
- Common cause: NVIDIA device plugin not running or RuntimeClass missing

### "nvidia-container-runtime: not found" in pod logs

- The NVIDIA runtime is not configured on the node
- Verify the containerd config and restart k3s/containerd

### Runner online but jobs stay pending

- Check runner labels match the job `runs-on` in `.github/workflows/ci.yml`
- HW test jobs need labels: `self-hosted`, `jetson`, AND `<variant>`
- Build/patch jobs use `ubuntu-latest` — no self-hosted runner needed

### GPU not visible inside pod

- Verify device plugin: `kubectl get pods -n kube-system | grep nvidia`
- Verify node resources: `kubectl describe node <node> | grep nvidia`
- Ensure `NVIDIA_VISIBLE_DEVICES=all` in runner env (the workflow sets this for test jobs)
