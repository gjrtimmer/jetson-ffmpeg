# Release-tools image for the GitLab release stage.
#
# Bundles pinned versions of the tools the release job needs:
#   - release-cli : create the GitLab Release
#   - git-cliff   : generate the changelog from Conventional Commits
#   - gh          : mirror the release to GitHub
#   - git, curl, jq, tar : glue
#
# Built + pushed to the project container registry by the `build:release-image`
# CI job (see .gitlab-ci.yml). Multi-arch aware: picks amd64/arm64 binaries
# based on the build host (so it works on shared runners or Jetson runners).
FROM debian:bookworm-slim

ARG RELEASE_CLI_VERSION=0.18.0
ARG GIT_CLIFF_VERSION=2.6.1
ARG GH_VERSION=2.62.0

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates curl jq git tar xz-utils \
    && rm -rf /var/lib/apt/lists/*

RUN set -eux; \
    arch="$(dpkg --print-architecture)"; \
    case "$arch" in \
        amd64) rcli=amd64; gc=x86_64;  gh=amd64 ;; \
        arm64) rcli=arm64; gc=aarch64; gh=arm64 ;; \
        *) echo "unsupported arch: $arch" >&2; exit 1 ;; \
    esac; \
    # release-cli
    curl -fsSL "https://gitlab.com/gitlab-org/release-cli/-/releases/v${RELEASE_CLI_VERSION}/downloads/bin/release-cli-linux-${rcli}" \
        -o /usr/local/bin/release-cli; \
    chmod +x /usr/local/bin/release-cli; \
    # git-cliff
    curl -fsSL "https://github.com/orhun/git-cliff/releases/download/v${GIT_CLIFF_VERSION}/git-cliff-${GIT_CLIFF_VERSION}-${gc}-unknown-linux-gnu.tar.gz" \
        | tar -xz -C /tmp; \
    mv /tmp/git-cliff-*/git-cliff /usr/local/bin/git-cliff; \
    chmod +x /usr/local/bin/git-cliff; \
    # gh
    curl -fsSL "https://github.com/cli/cli/releases/download/v${GH_VERSION}/gh_${GH_VERSION}_linux_${gh}.tar.gz" \
        | tar -xz -C /tmp; \
    mv /tmp/gh_*/bin/gh /usr/local/bin/gh; \
    chmod +x /usr/local/bin/gh; \
    rm -rf /tmp/git-cliff-* /tmp/gh_*; \
    # sanity
    release-cli --version; git-cliff --version; gh --version

WORKDIR /workspace
