#!/usr/bin/env bash
# Create the release: changelog -> S3 upload -> GitLab Release (+ generic
# package assets). Intended to run inside the release-tools image on a tag
# pipeline (see .gitlab-ci.yml "release" job). Not meant for local use, but
# every external dependency is an env var so it can be dry-run.
#
# Required (provided by GitLab CI on a tag pipeline):
#   CI_COMMIT_TAG, CI_API_V4_URL, CI_PROJECT_ID, CI_JOB_TOKEN, CI_PROJECT_URL
# Required (S3/MinIO upload):
#   S3_HOST          - MinIO endpoint (https://... or http://... for in-cluster)
#   S3_CLIENT_ID     - MinIO access key
#   S3_CLIENT_SECRET - MinIO secret key
#   S3_BUCKET        - target bucket name
# Inputs:
#   dist/*.tar.gz - per-version archives produced by scripts/package.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${REPO_ROOT}"

TAG="${CI_COMMIT_TAG:?CI_COMMIT_TAG is required (run on a tag pipeline)}"
VERSION="${TAG#v}"
DIST="${REPO_ROOT}/dist"

echo "=== releasing ${TAG} (version ${VERSION}) ==="

# --- 1. changelog --------------------------------------------------------
# git-cliff needs full history + tags; the CI job fetches them (GIT_DEPTH: 0).
echo "[i] generating changelog with git-cliff"
git-cliff --config cliff.toml --latest --strip header -o NOTES.md || {
    echo "[!] git-cliff failed; falling back to raw git log"
    PREV_TAG="$(git describe --tags --abbrev=0 "${TAG}^" 2>/dev/null || true)"
    { echo "## ${VERSION}"; echo;
      if [ -n "${PREV_TAG}" ]; then git log --pretty='- %s' "${PREV_TAG}..${TAG}";
      else git log --pretty='- %s' "${TAG}"; fi; } > NOTES.md
}
echo "----- NOTES.md -----"; cat NOTES.md; echo "--------------------"

# --- 2. upload archives + changelog to S3 (MinIO) -----------------------
shopt -s nullglob
ARCHIVES=( "${DIST}"/*.tar.gz )
if [ ${#ARCHIVES[@]} -eq 0 ]; then echo "[E] no archives in ${DIST}" >&2; exit 1; fi

: "${S3_HOST:?S3_HOST is required}"
: "${S3_CLIENT_ID:?S3_CLIENT_ID is required}"
: "${S3_CLIENT_SECRET:?S3_CLIENT_SECRET is required}"
: "${S3_BUCKET:?S3_BUCKET is required}"

# Determine secure/insecure based on endpoint scheme
MC_SECURE="true"
if [[ "${S3_HOST}" == http://* ]]; then
    MC_SECURE="false"
    echo "[i] S3 endpoint uses http — insecure mode (in-cluster)"
fi

# Configure mc alias for this pipeline run
ALIAS_FLAGS=(--api S3v4 --path auto)
[ "${MC_SECURE}" = "false" ] && ALIAS_FLAGS+=(--insecure)
mc alias set s3 "${S3_HOST}" "${S3_CLIENT_ID}" "${S3_CLIENT_SECRET}" \
    "${ALIAS_FLAGS[@]}" 2>/dev/null

S3_PREFIX="s3/${S3_BUCKET}/releases/${VERSION}"
MC_FLAGS=()
[ "${MC_SECURE}" = "false" ] && MC_FLAGS+=(--insecure)

echo "[i] uploading ${#ARCHIVES[@]} archives to ${S3_PREFIX}/"
for f in "${ARCHIVES[@]}"; do
    name="$(basename "$f")"
    echo "  -> ${name}"
    mc cp "${MC_FLAGS[@]}" "$f" "${S3_PREFIX}/${name}"
done

echo "[i] uploading CHANGELOG.md to ${S3_PREFIX}/"
mc cp "${MC_FLAGS[@]}" NOTES.md "${S3_PREFIX}/CHANGELOG.md"

echo "[i] verifying S3 upload"
mc ls "${MC_FLAGS[@]}" "${S3_PREFIX}/" | head -20

# --- 3. upload archives to GitLab generic package registry ---------------

ASSET_ARGS=()
PKG_BASE="${CI_API_V4_URL}/projects/${CI_PROJECT_ID}/packages/generic/jetson-ffmpeg/${VERSION}"
for f in "${ARCHIVES[@]}"; do
    name="$(basename "$f")"
    url="${PKG_BASE}/${name}"
    echo "[i] uploading ${name} -> generic package registry"
    curl --fail --silent --show-error \
         --header "JOB-TOKEN: ${CI_JOB_TOKEN}" \
         --upload-file "$f" "$url"
    ASSET_ARGS+=( --assets-link "{\"name\":\"${name}\",\"url\":\"${url}\",\"link_type\":\"package\"}" )
done

# --- 4. GitLab release ---------------------------------------------------
# Idempotent: delete any existing release for this tag before creating.
# Retried pipelines and manual re-runs hit a 409 "Release already exists"
# without this. Packages (step 2) are registry-scoped and unaffected.
RELEASE_API="${CI_API_V4_URL}/projects/${CI_PROJECT_ID}/releases/${TAG}"
if curl --silent --fail --head --header "JOB-TOKEN: ${CI_JOB_TOKEN}" "${RELEASE_API}" >/dev/null 2>&1; then
    echo "[i] release ${TAG} already exists — deleting before recreate"
    curl --silent --fail --request DELETE \
         --header "JOB-TOKEN: ${CI_JOB_TOKEN}" "${RELEASE_API}" || \
        echo "[!] delete failed (may need MAINTAINER token); continuing anyway"
fi

echo "[i] creating GitLab release ${TAG}"
release-cli create \
    --name "jetson-ffmpeg ${VERSION}" \
    --tag-name "${TAG}" \
    --description "NOTES.md" \
    "${ASSET_ARGS[@]}"


echo "=== release ${TAG} done ==="
