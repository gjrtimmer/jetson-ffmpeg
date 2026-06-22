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
#   l4t-jetpack6    Base JetPack 6 image with dpkg conffile fix
#   builder-jp6     CI builder image for JP6 (depends on l4t-jetpack6)
#   l4t-jetpack5    Base JetPack 5 image (compile-test only)
#   builder-jp5     CI builder image for JP5 compile-test (depends on l4t-jetpack5)
#   release-tools   Release stage tooling (git-cliff, gh, release-cli)
#   all             All images in dependency order (default)
#
# Options:
#   --l4t-tag TAG       L4T version tag for JP6 (default: r36.4.0)
#   --l4t-tag-jp5 TAG   L4T version tag for JP5 (default: r35.6.0)
#   --dockerhub USER    DockerHub username (default: $DOCKERHUB_USERNAME)
#   --no-push           Build only, do not push
#   --dry-run           Print commands without executing
#   -h, --help          Show this help
#
# Examples:
#   scripts/build-images.sh                        # build + push all
#   scripts/build-images.sh l4t-jetpack6           # only l4t-jetpack6
#   scripts/build-images.sh --no-push builder-jp6  # build builder-jp6 locally
#   scripts/build-images.sh l4t-jetpack5 builder-jp5  # only JP5 images
#   scripts/build-images.sh release-tools          # only release-tools

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

L4T_TAG="r36.4.0"
L4T_TAG_JP5="r35.2.1"
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
        --l4t-tag)      L4T_TAG="$2"; shift 2 ;;
        --l4t-tag-jp5)  L4T_TAG_JP5="$2"; shift 2 ;;
        --dockerhub)    DOCKERHUB_USERNAME="$2"; shift 2 ;;
        --no-push)      PUSH=false; shift ;;
        --dry-run)      DRY_RUN=true; shift ;;
        -h|--help)      usage ;;
        -*)             die "unknown option: $1" ;;
        *)              IMAGES+=("$1"); shift ;;
    esac
done

[[ ${#IMAGES[@]} -eq 0 ]] && IMAGES=(all)
[[ "${IMAGES[*]}" == "all" ]] && IMAGES=(l4t-jetpack6 builder-jp6 l4t-jetpack5 builder-jp5 release-tools)

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
        --build-arg "L4T_TAG=${CUR_L4T_TAG}" \
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
        l4t-jetpack6)
            CUR_L4T_TAG="${L4T_TAG}"
            build_and_push "l4t-jetpack6" "ci/l4t-jetpack6.Dockerfile" \
                "harbor.local/jetson/l4t-jetpack6:${L4T_TAG}" \
                "docker.io/${DOCKERHUB_USERNAME}/l4t-jetpack6:${L4T_TAG}"
            ;;
        builder-jp6)
            CUR_L4T_TAG="${L4T_TAG}"
            build_and_push "builder-jp6" "ci/builder-jp6.Dockerfile" \
                "harbor.local/jetson/jetson-ffmpeg-builder:${L4T_TAG}" \
                "docker.io/${DOCKERHUB_USERNAME}/jetson-ffmpeg-builder:${L4T_TAG}"
            ;;
        l4t-jetpack5)
            CUR_L4T_TAG="${L4T_TAG_JP5}"
            build_and_push "l4t-jetpack5" "ci/l4t-jetpack5.Dockerfile" \
                "harbor.local/jetson/l4t-jetpack5:${L4T_TAG_JP5}" \
                "docker.io/${DOCKERHUB_USERNAME}/l4t-jetpack5:${L4T_TAG_JP5}"
            ;;
        builder-jp5)
            CUR_L4T_TAG="${L4T_TAG_JP5}"
            build_and_push "builder-jp5" "ci/builder-jp5.Dockerfile" \
                "harbor.local/jetson/jetson-ffmpeg-builder-jp5:${L4T_TAG_JP5}" \
                "docker.io/${DOCKERHUB_USERNAME}/jetson-ffmpeg-builder-jp5:${L4T_TAG_JP5}"
            ;;
        release-tools)
            CUR_L4T_TAG="${L4T_TAG}"
            build_and_push "release-tools" "ci/release-tools.Dockerfile" \
                "harbor.local/jetson/jetson-ffmpeg-release-tools:latest" \
                "docker.io/${DOCKERHUB_USERNAME}/jetson-ffmpeg-release-tools:latest"
            ;;
        *)
            die "unknown image: ${img} (expected: l4t-jetpack6, builder-jp6, l4t-jetpack5, builder-jp5, release-tools, all)"
            ;;
    esac
done

echo "All done."
