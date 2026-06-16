## GitLab Runner Token Request

**I would like to contribute a Jetson runner to the GitLab CI pipeline.**

### Hardware

- **Jetson model**: <!-- e.g. Orin NX, AGX Orin, Orin Nano, Xavier NX -->
- **Proposed variant tag**: <!-- e.g. orin-nx, agx, nano — used alongside the "jetson" tag -->
- **JetPack / L4T version**: <!-- e.g. JetPack 6.1 / L4T r36.4.0 -->

### Runner environment

- [ ] Kubernetes (k3s / k8s)
- [ ] Bare-metal (directly on Jetson OS)

### Checklist

- [ ] NVIDIA Container Runtime is installed and working
- [ ] I have verified GPU access (`nvidia-smi` or `tegrastats` works)
- [ ] I have read the runner setup guide ([Kubernetes](https://github.com/gjrtimmer/jetson-ffmpeg/wiki/GitLab-Runner-Kubernetes) or [Manual](https://github.com/gjrtimmer/jetson-ffmpeg/wiki/GitLab-Runner-Manual))

### Additional context

<!-- Any relevant details: network constraints, availability schedule, etc. -->

---

**For maintainers:** after approving, create a project runner at **Settings > CI/CD > Runners > New project runner** with tags `jetson, <variant>` and share the `glrt-...` token with the requester via a secure channel.

/label ~runner-request
