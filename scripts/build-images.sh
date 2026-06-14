#!/bin/bash
# Build and push CI Docker images from a local machine (e.g. Mac with Apple
# Silicon). Faster than the in-cluster buildkit pipeline for large images.
#
# Pushes to both Harbor (harbor.local) and DockerHub.
#
# Usage:
#   scripts/build-images.sh [options] [image...]
#
# Images:
#   l4t-jetpack     Base JetPack image with dpkg conffile fix
#   builder         CI builder image (depends on l4t-jetpack)
#   release-tools   Release stage tooling (git-cliff, gh, release-cli)
#   all             All images in dependency order (default)
#
# Options:
#   --l4t-tag TAG       L4T version tag (default: r36.4.0)
#   --dockerhub USER    DockerHub username (default: $DOCKERHUB_USERNAME)
#   --no-push           Build only, do not push
#   --dry-run           Print commands without executing
#   -h, --help          Show this help
#
# Examples:
#   scripts/build-images.sh                        # build + push all
#   scripts/build-images.sh l4t-jetpack            # only l4t-jetpack
#   scripts/build-images.sh --no-push builder      # build builder locally
#   scripts/build-images.sh release-tools          # only release-tools

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

L4T_TAG="r36.4.0"
DOCKERHUB_USERNAME="${DOCKERHUB_USERNAME:-}"
PUSH=true
DRY_RUN=false
IMAGES=()

die() { echo "error: $*" >&2; exit 1; }

usage() {
    sed -n '2,/^$/{ s/^# \?//; p }' "$0"
    exit 0
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --l4t-tag)    L4T_TAG="$2"; shift 2 ;;
        --dockerhub)  DOCKERHUB_USERNAME="$2"; shift 2 ;;
        --no-push)    PUSH=false; shift ;;
        --dry-run)    DRY_RUN=true; shift ;;
        -h|--help)    usage ;;
        -*)           die "unknown option: $1" ;;
        *)            IMAGES+=("$1"); shift ;;
    esac
done

[[ ${#IMAGES[@]} -eq 0 ]] && IMAGES=(all)
[[ "${IMAGES[*]}" == "all" ]] && IMAGES=(l4t-jetpack builder release-tools)

if $PUSH && [[ -z "$DOCKERHUB_USERNAME" ]]; then
    die "set DOCKERHUB_USERNAME env var or pass --dockerhub USER for DockerHub push"
fi

run() {
    echo "+ $*"
    $DRY_RUN || "$@"
}

build_and_push() {
    local name="$1" dockerfile="$2"
    shift 2
    local tags=("$@")

    echo "=== Building ${name} ==="
    local tag_args=()
    for t in "${tags[@]}"; do
        tag_args+=(-t "${t}")
    done

    run docker build --platform linux/arm64 \
        -f "${REPO_ROOT}/${dockerfile}" \
        --build-arg "L4T_TAG=${L4T_TAG}" \
        "${tag_args[@]}" \
        "${REPO_ROOT}"

    if $PUSH; then
        echo "=== Pushing ${name} ==="
        for t in "${tags[@]}"; do
            run docker push "${t}"
        done
    fi

    echo "=== ${name} done ==="
    echo
}

for img in "${IMAGES[@]}"; do
    case "$img" in
        l4t-jetpack)
            build_and_push "l4t-jetpack" "ci/l4t-jetpack.Dockerfile" \
                "harbor.local/jetson/l4t-jetpack:${L4T_TAG}" \
                "docker.io/${DOCKERHUB_USERNAME}/l4t-jetpack:${L4T_TAG}"
            ;;
        builder)
            build_and_push "builder" "ci/builder.Dockerfile" \
                "harbor.local/jetson/jetson-ffmpeg-builder:${L4T_TAG}" \
                "docker.io/${DOCKERHUB_USERNAME}/jetson-ffmpeg-builder:${L4T_TAG}"
            ;;
        release-tools)
            build_and_push "release-tools" "ci/release-tools.Dockerfile" \
                "harbor.local/jetson/jetson-ffmpeg-release-tools:latest" \
                "docker.io/${DOCKERHUB_USERNAME}/jetson-ffmpeg-release-tools:latest"
            ;;
        *)
            die "unknown image: ${img} (expected: l4t-jetpack, builder, release-tools, all)"
            ;;
    esac
done

echo "All done."
