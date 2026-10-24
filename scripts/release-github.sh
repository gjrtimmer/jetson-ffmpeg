#!/usr/bin/env bash
# Create (or update) the GitHub Release for a tag and upload the release assets.
# Intended for the CI "release:github" job (release-tools image: curl + jq).
# The GitHub repo is a GitLab push-mirror, so the tag arrives there
# asynchronously: step 1 waits for it. Every external dependency is an env var
# so it can be run against a fake API (test/integration/release-github-fake.sh).
#
# Required (CI/CD variables; missing = hard failure):
#   CI_COMMIT_TAG, CI_COMMIT_SHA, GITHUB_TOKEN (masked CI/CD variable, contents:write)
# Optional:
#   GITHUB_REPO          owner/repo (default gjrtimmer/jetson-ffmpeg; CI/CD variable overrides)
#   GITHUB_API           API base (default https://api.github.com)
#   GITHUB_UPLOADS       upload base (default https://uploads.github.com)
#   MIRROR_WAIT_TIMEOUT  seconds to wait for the mirrored tag (default 600)
#   MIRROR_POLL_INTERVAL seconds between polls (default 20)
# Inputs:
#   dist/*.tar.gz              per-version archives from the dist jobs
#   NOTES.md                   release body, generated here by scripts/changelog.sh
#                              (git-cliff; same notes as the GitLab release)
#                              NOTES.md is also uploaded as the CHANGELOG.md asset,
#                              identical to the GitLab/S3 release asset
#
# The token is never put on argv, in a URL, or in the log: every request reads
# its headers from a curl config on stdin. Never run this with `set -x`.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${REPO_ROOT}"

TAG="${CI_COMMIT_TAG:?CI_COMMIT_TAG is required (run on a tag pipeline)}"
COMMIT_SHA="${CI_COMMIT_SHA:?CI_COMMIT_SHA is required}"
: "${GITHUB_TOKEN:?GITHUB_TOKEN is required}"
VERSION="${TAG#v}"
GITHUB_REPO="${GITHUB_REPO:-gjrtimmer/jetson-ffmpeg}"
GITHUB_API="${GITHUB_API:-https://api.github.com}"
GITHUB_UPLOADS="${GITHUB_UPLOADS:-https://uploads.github.com}"
MIRROR_WAIT_TIMEOUT="${MIRROR_WAIT_TIMEOUT:-600}"
MIRROR_POLL_INTERVAL="${MIRROR_POLL_INTERVAL:-20}"
DIST="${REPO_ROOT}/dist"

BODY_FILE="$(mktemp)"
trap 'rm -f "${BODY_FILE}"' EXIT

# gh_curl <curl args...>: authenticated request. Headers (incl. the token)
# come from a curl config on stdin, so they never show up in `ps`. The HTTP
# status goes to stdout after the body file is written to ${BODY_FILE}.
gh_curl() {
    curl --silent --show-error --config - --output "${BODY_FILE}" \
         --write-out '%{http_code}' "$@" <<CFG
header = "Authorization: Bearer ${GITHUB_TOKEN}"
header = "Accept: application/vnd.github+json"
header = "X-GitHub-Api-Version: 2022-11-28"
CFG
}

# fail_http <what> <status>: print status + GitHub's error message (not the
# request, so no secrets) and exit.
fail_http() {
    local msg
    msg="$(jq -r '.message // empty' "${BODY_FILE}" 2>/dev/null || true)"
    echo "[E] ${1}: HTTP ${2} ${msg}" >&2
    exit 1
}

# --- 1. wait for the mirrored tag, verify it points at this commit --------
wait_for_tag() {
    local deadline code obj_type obj_sha
    deadline=$(( $(date +%s) + MIRROR_WAIT_TIMEOUT ))
    echo "[i] waiting for tag ${TAG} on ${GITHUB_REPO} (timeout ${MIRROR_WAIT_TIMEOUT}s)"
    while :; do
        code="$(gh_curl "${GITHUB_API}/repos/${GITHUB_REPO}/git/ref/tags/${TAG}")"
        if [ "${code}" = "200" ]; then break; fi
        # 404 = mirror has not delivered yet; anything else is a real error.
        if [ "${code}" != "404" ]; then fail_http "tag lookup" "${code}"; fi
        if [ "$(date +%s)" -ge "${deadline}" ]; then
            echo "[E] tag ${TAG} not on ${GITHUB_REPO} after ${MIRROR_WAIT_TIMEOUT}s (push mirror stuck?)" >&2
            exit 1
        fi
        sleep "${MIRROR_POLL_INTERVAL}"
    done

    obj_type="$(jq -r '.object.type' "${BODY_FILE}")"
    obj_sha="$(jq -r '.object.sha' "${BODY_FILE}")"
    # Annotated tags point at a tag object; deref it to the commit.
    if [ "${obj_type}" = "tag" ]; then
        code="$(gh_curl "${GITHUB_API}/repos/${GITHUB_REPO}/git/tags/${obj_sha}")"
        if [ "${code}" != "200" ]; then fail_http "tag object lookup" "${code}"; fi
        obj_sha="$(jq -r '.object.sha' "${BODY_FILE}")"
    fi
    if [ "${obj_sha}" != "${COMMIT_SHA}" ]; then
        echo "[E] GitHub tag ${TAG} is ${obj_sha}, pipeline commit is ${COMMIT_SHA}" >&2
        exit 1
    fi
    echo "[i] tag ${TAG} present on GitHub at ${obj_sha}"
}

# --- 2. idempotent release ------------------------------------------------
# Echoes the release id. Existing release: PATCH name/body, else POST.
ensure_release() {
    local payload code id
    # jq --rawfile escapes the notes body safely.
    payload="$(jq -n --arg tag "${TAG}" --arg name "v${VERSION}" \
        --rawfile body NOTES.md \
        '{tag_name:$tag, name:$name, body:$body, draft:false,
          prerelease:false, make_latest:"true"}')"

    code="$(gh_curl "${GITHUB_API}/repos/${GITHUB_REPO}/releases/tags/${TAG}")"
    if [ "${code}" = "200" ]; then
        id="$(jq -r '.id' "${BODY_FILE}")"
        echo "[i] release ${TAG} exists (id ${id}) - updating" >&2
        code="$(gh_curl --request PATCH --data "${payload}" \
            "${GITHUB_API}/repos/${GITHUB_REPO}/releases/${id}")"
        if [ "${code}" != "200" ]; then fail_http "release update" "${code}"; fi
    elif [ "${code}" = "404" ]; then
        echo "[i] creating GitHub release ${TAG}" >&2
        code="$(gh_curl --request POST --data "${payload}" \
            "${GITHUB_API}/repos/${GITHUB_REPO}/releases")"
        if [ "${code}" != "201" ]; then fail_http "release create" "${code}"; fi
    else
        fail_http "release lookup" "${code}"
    fi
    jq -r '.id' "${BODY_FILE}"
}

# GitHub normalizes asset names (e.g. `~` becomes `.`), so the existing-asset
# match uses the same normalization: both the stored name and the local name
# are compared after mapping every char outside [A-Za-z0-9._+-] to `.`.
norm_name() { printf '%s' "$1" | sed 's/[^A-Za-z0-9._+-]/./g'; }

content_type() {
    case "$1" in
        *.tar.gz) echo application/gzip ;;
        *.md) echo text/markdown ;;
        *) echo application/octet-stream ;;
    esac
}

# upload_asset <release-id> <file> <asset-name>
upload_asset() {
    local id="$1" file="$2" name="$3" code existing_id enc stored
    # Delete a same-named asset first so re-runs replace it (GitHub 422s on
    # duplicates). Listing is captured to a variable before filtering.
    code="$(gh_curl "${GITHUB_API}/repos/${GITHUB_REPO}/releases/${id}/assets?per_page=100")"
    if [ "${code}" != "200" ]; then fail_http "asset list" "${code}"; fi
    existing_id="$(jq -r --arg n "$(norm_name "${name}")" \
        '[.[] | select(.name == $n or .name == ($n | gsub("[^A-Za-z0-9._+-]"; "."))) | .id][0] // empty' \
        "${BODY_FILE}")"
    if [ -n "${existing_id}" ]; then
        echo "[i] replacing existing asset ${name} (id ${existing_id})"
        code="$(gh_curl --request DELETE \
            "${GITHUB_API}/repos/${GITHUB_REPO}/releases/assets/${existing_id}")"
        if [ "${code}" != "204" ]; then fail_http "asset delete" "${code}"; fi
    fi

    enc="$(jq -rn --arg n "${name}" '$n | @uri')"
    echo "[i] uploading ${name} ($(content_type "${name}"))"
    code="$(gh_curl --request POST \
        --header "Content-Type: $(content_type "${name}")" \
        --data-binary "@${file}" \
        "${GITHUB_UPLOADS}/repos/${GITHUB_REPO}/releases/${id}/assets?name=${enc}")"
    if [ "${code}" != "201" ]; then fail_http "asset upload ${name}" "${code}"; fi
    stored="$(jq -r '.name' "${BODY_FILE}")"
    # GitHub may rename (e.g. `~` -> `.`); log it, do not fail.
    if [ "${stored}" != "${name}" ]; then
        echo "[i] GitHub stored ${name} as ${stored}"
    else
        echo "[i] stored as ${stored}"
    fi
}

main() {
    echo "=== GitHub release ${TAG} (${GITHUB_REPO}) ==="
    # Independent of the GitLab release job, so generate the notes here.
    bash "${SCRIPT_DIR}/changelog.sh"
    shopt -s nullglob
    local files=( "${DIST}"/*.tar.gz )
    if [ ${#files[@]} -eq 0 ]; then echo "[E] no archives in ${DIST}" >&2; exit 1; fi

    wait_for_tag
    local id f
    id="$(ensure_release)"
    for f in "${files[@]}"; do
        upload_asset "${id}" "${f}" "$(basename "${f}")"
    done
    upload_asset "${id}" NOTES.md "CHANGELOG.md"
    echo "=== GitHub release ${TAG} done ==="
}

main "$@"
