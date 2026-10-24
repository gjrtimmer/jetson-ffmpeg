#!/usr/bin/env bash
# Generate the release notes (NOTES.md) for the current tag with git-cliff.
# git-cliff needs full history + tags (CI jobs set GIT_DEPTH: 0).
#
# Required: CI_COMMIT_TAG
# Output:   NOTES.md in the repo root
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}/.."

TAG="${CI_COMMIT_TAG:?CI_COMMIT_TAG is required (run on a tag pipeline)}"
VERSION="${TAG#v}"

echo "[i] generating changelog with git-cliff"
git-cliff --config cliff.toml --latest --strip header -o NOTES.md || {
    echo "[!] git-cliff failed; falling back to raw git log"
    PREV_TAG="$(git describe --tags --abbrev=0 "${TAG}^" 2>/dev/null || true)"
    { echo "## ${VERSION}"; echo;
      if [ -n "${PREV_TAG}" ]; then git log --pretty='- %s' "${PREV_TAG}..${TAG}";
      else git log --pretty='- %s' "${TAG}"; fi; } > NOTES.md
}
echo "----- NOTES.md -----"; cat NOTES.md; echo "--------------------"
