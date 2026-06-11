## GitHub Runner Token Request

**I would like to contribute a self-hosted Jetson runner to the GitHub Actions pipeline.**

### Hardware

- **Jetson model**: <!-- e.g. Orin NX, AGX Orin, Orin Nano, Xavier NX -->
- **Proposed variant label**: <!-- e.g. orin-nx, agx, nano — used alongside the "jetson" label -->
- **JetPack / L4T version**: <!-- e.g. JetPack 6.1 / L4T r36.4.0 -->

### Runner environment

- [ ] Kubernetes (k3s / k8s)
- [ ] Bare-metal (directly on Jetson OS)

### Checklist

- [ ] NVIDIA Container Runtime / Docker with nvidia runtime is installed
- [ ] I have verified GPU access (`nvidia-smi` or `tegrastats` works)
- [ ] I have read the runner setup guide ([Kubernetes](docs/GITHUB_RUNNER_K8S.md) or [Manual](docs/GITHUB_RUNNER_MANUAL.md))

### GitHub username

<!-- Your GitHub username so the maintainer can grant runner access -->

### Additional context

<!-- Any relevant details: network constraints, availability schedule, etc. -->

---

**For maintainers:** after approving, add the requester's GitHub account to the repository runner group and share the registration token. The requester registers the runner with labels `self-hosted, jetson, <variant>`.

/label ~runner-request
