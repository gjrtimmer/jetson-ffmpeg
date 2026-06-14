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
#   l4t-jetpack   Base JetPack image with dpkg conffile fix
#   builder       CI builder image (depends on l4t-jetpack)
#   all           Both images in order (default)
#
# Options:
#   --l4t-tag TAG       L4T version tag (default: r36.4.0)
#   --dockerhub USER    DockerHub username (default: $DOCKERHUB_USERNAME)
#   --no-push           Build only, do not push
#   --dry-run           Print commands without executing
#   -h, --help          Show this help
#
# Examples:
#   scripts/build-images.sh                    # build + push all
#   scripts/build-images.sh l4t-jetpack        # only l4t-jetpack
#   scripts/build-images.sh --no-push builder  # build builder locally

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
[[ "${IMAGES[*]}" == "all" ]] && IMAGES=(l4t-jetpack builder)

if $PUSH && [[ -z "$DOCKERHUB_USERNAME" ]]; then
    die "set DOCKERHUB_USERNAME env var or pass --dockerhub USER for DockerHub push"
fi

run() {
    echo "+ $*"
    $DRY_RUN || "$@"
}

HARBOR_L4T="harbor.local/jetson/l4t-jetpack:${L4T_TAG}"
HARBOR_BUILDER="harbor.local/jetson/jetson-ffmpeg-builder:${L4T_TAG}"
DOCKERHUB_L4T="docker.io/${DOCKERHUB_USERNAME}/l4t-jetpack:${L4T_TAG}"
DOCKERHUB_BUILDER="docker.io/${DOCKERHUB_USERNAME}/jetson-ffmpeg-builder:${L4T_TAG}"

build_image() {
    local name="$1" dockerfile="$2" tag="$3" dockerhub_tag="$4"

    echo "=== Building ${name} ==="
    run docker build --platform linux/arm64 \
        -f "${REPO_ROOT}/${dockerfile}" \
        --build-arg "L4T_TAG=${L4T_TAG}" \
        -t "${tag}" \
        "${REPO_ROOT}"

    if $PUSH; then
        echo "=== Pushing ${name} ==="
        run docker push "${tag}"
        if [[ -n "$dockerhub_tag" ]]; then
            run docker tag "${tag}" "${dockerhub_tag}"
            run docker push "${dockerhub_tag}"
        fi
    fi

    echo "=== ${name} done ==="
    echo
}

for img in "${IMAGES[@]}"; do
    case "$img" in
        l4t-jetpack)
            build_image "l4t-jetpack" "ci/l4t-jetpack.Dockerfile" \
                "${HARBOR_L4T}" "${DOCKERHUB_L4T}"
            ;;
        builder)
            build_image "builder" "ci/builder.Dockerfile" \
                "${HARBOR_BUILDER}" "${DOCKERHUB_BUILDER}"
            ;;
        *)
            die "unknown image: ${img} (expected: l4t-jetpack, builder, all)"
            ;;
    esac
done

echo "All done."
