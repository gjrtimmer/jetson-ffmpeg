#!/usr/bin/env bash
# Install macOS development tools with Homebrew (idempotent, safe to re-run).
# Building/testing needs a Jetson; these are the host-side tools:
# release (git-cliff), GitLab/GitHub CLIs, linting.
#
# Usage: ./scripts/install-dev-deps.sh
set -euo pipefail

FORMULAE=( git-cliff glab gh jq shellcheck actionlint )

command -v brew >/dev/null 2>&1 || { echo "[E] Homebrew not found: https://brew.sh" >&2; exit 1; }

for f in "${FORMULAE[@]}"; do
    if brew list --formula "$f" >/dev/null 2>&1; then
        echo "[i] $f already installed"
    else
        echo "[i] installing $f"
        brew install "$f"
    fi
done

echo "[i] done. Authenticate if needed: glab auth login, gh auth login"
