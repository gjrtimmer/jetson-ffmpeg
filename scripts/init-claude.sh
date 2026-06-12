#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# init-claude.sh — post-login Claude Code setup
#
# Run ONCE after first 'claude' login in the devcontainer.
# Installs skills, plugins, and tools that require ~/.claude to exist.
#
# Usage:  ./scripts/init-claude.sh
#         init-claude              (alias)
# -----------------------------------------------------------------------------
set -euo pipefail

GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
BOLD='\033[1m'
DIM='\033[2m'
NC='\033[0m'

STEPS=9
CURRENT=0
PASS=0
FAIL=0

step() {
    CURRENT=$((CURRENT + 1))
    printf '\n%b[%d/%d]%b %b%s%b\n' "$BOLD" "$CURRENT" "$STEPS" "$NC" "$BOLD" "$1" "$NC"
}

progress() { printf '  %b→ %s%b\n' "$DIM" "$1" "$NC"; }
ok()       { PASS=$((PASS + 1)); printf '  %b✓ %s%b\n' "$GREEN" "$1" "$NC"; }
skip()     { printf '  %b⊘ %s%b\n' "$YELLOW" "$1" "$NC"; }
err()      { FAIL=$((FAIL + 1)); printf '  %b✗ %s%b\n' "$RED" "$1" "$NC"; }

# ---------------------------------------------------------------------------
# Prerequisites
# ---------------------------------------------------------------------------
printf '%b=== Claude Code Init ===%b\n' "$BOLD" "$NC"
printf 'Checking prerequisites...\n'

missing=0
for cmd in node npx gh claude; do
    if command -v "$cmd" &>/dev/null; then
        ok "$cmd found"
    else
        err "$cmd not found in PATH"
        missing=1
    fi
done

if [[ ! -d "${HOME}/.claude" ]]; then
    err "$HOME/.claude not found — run 'claude' once to login first"
    exit 1
fi
ok "$HOME/.claude exists"

(( missing )) && exit 1

# ---------------------------------------------------------------------------
# 1. uv / uvx — Python package manager (user-level install)
# https://docs.astral.sh/uv/
# ---------------------------------------------------------------------------
step "uv / uvx — Python package manager"
if command -v uv &>/dev/null; then
    skip "uv already installed ($(uv --version))"
else
    progress "curl -LsSf https://astral.sh/uv/install.sh | sh"
    if curl -LsSf https://astral.sh/uv/install.sh | sh; then
        export PATH="${HOME}/.local/bin:${PATH}"
        ok "uv installed ($(uv --version))"
    else
        err "uv install failed (non-fatal)"
    fi
fi

# ---------------------------------------------------------------------------
# 2. Caveman — token-efficient communication mode
# https://github.com/JuliusBrussee/caveman
# ---------------------------------------------------------------------------
step "Caveman — token-efficient communication"
progress "Downloading installer..."
if curl -fsSL https://raw.githubusercontent.com/JuliusBrussee/caveman/main/install.sh | bash; then
    ok "Caveman installed"
else
    err "Caveman install failed (non-fatal)"
fi

# ---------------------------------------------------------------------------
# 3. Ruflo — AI agent orchestration
# https://github.com/ruvnet/claude-flow
# ---------------------------------------------------------------------------
step "Ruflo — agent orchestration"
progress "npm install -g ruflo@latest"
if npm install -g ruflo@latest; then
    ok "Ruflo installed globally"
else
    err "Ruflo install failed (non-fatal)"
fi

# ---------------------------------------------------------------------------
# 4. Semble — fast code search for agents
# https://github.com/MinishLab/semble
# ---------------------------------------------------------------------------
step "Semble — fast code search"
if command -v uv &>/dev/null; then
    progress "uv tool install semble"
    if uv tool install semble; then
        ok "Semble installed"
    else
        err "Semble install failed (non-fatal)"
    fi
else
    skip "uv not found — skipping Semble"
fi

# ---------------------------------------------------------------------------
# 5. GH Issues Auto-Fixer — automated issue-to-PR skill
# https://github.com/openclaw/openclaw (skills/gh-issues)
# ---------------------------------------------------------------------------
step "GH Issues Auto-Fixer — issue-to-PR automation"
progress "gh skill install openclaw/openclaw gh-issues"
if gh skill install openclaw/openclaw gh-issues; then
    ok "gh-issues skill installed"
else
    err "gh skill install failed"
    progress "Fallback: npx skills add https://github.com/openclaw/openclaw --skill gh-issues"
fi

# ---------------------------------------------------------------------------
# 6. SuperPowers — agentic skills framework
# https://github.com/obra/superpowers
# ---------------------------------------------------------------------------
step "SuperPowers — agentic skills framework"
progress "claude plugin install superpowers@claude-plugins-official"
if claude plugin install superpowers@claude-plugins-official; then
    ok "SuperPowers installed"
else
    err "SuperPowers install failed (non-fatal)"
fi

# ---------------------------------------------------------------------------
# 7. Context Mode — context window optimization
# https://github.com/mksglu/context-mode
# ---------------------------------------------------------------------------
step "Context Mode — context window optimization"
progress "Adding marketplace mksglu/context-mode..."
claude plugin marketplace add mksglu/context-mode || true
progress "claude plugin install context-mode@context-mode"
if claude plugin install context-mode@context-mode; then
    ok "Context Mode installed"
else
    err "Context Mode install failed (non-fatal)"
fi

# ---------------------------------------------------------------------------
# 8. Fullstack Dev Skills — 66 specialized dev skills
# https://github.com/Jeffallan/claude-skills
# ---------------------------------------------------------------------------
step "Fullstack Dev Skills — 66 specialized skills"
progress "Adding marketplace jeffallan/claude-skills..."
claude plugin marketplace add jeffallan/claude-skills || true
progress "Updating marketplace fullstack-dev-skills..."
claude plugin marketplace update fullstack-dev-skills || true
progress "claude plugin install fullstack-dev-skills@fullstack-dev-skills"
if claude plugin install fullstack-dev-skills@fullstack-dev-skills; then
    ok "Fullstack Dev Skills installed"
else
    err "Fullstack Dev Skills install failed (non-fatal)"
fi

# ---------------------------------------------------------------------------
# 9. Environment check
# ---------------------------------------------------------------------------
step "Environment check"
if [[ -n "${GITHUB_TOKEN:-}" ]]; then
    ok "GITHUB_TOKEN set — GitHub MCP server ready"
else
    skip "GITHUB_TOKEN not set — GitHub MCP server will not authenticate"
    progress "Set on host: export GITHUB_TOKEN=ghp_..."
    progress "Devcontainer passes it through via \${localEnv:GITHUB_TOKEN}"
fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
printf '\n%b========================================%b\n' "$BOLD" "$NC"
printf '%b  Done: %b%d passed%b, %b%d failed%b, of %d steps%b\n' \
    "$BOLD" "$GREEN" "$PASS" "$NC" "$RED" "$FAIL" "$NC" "$STEPS" "$NC"
printf '%b========================================%b\n' "$BOLD" "$NC"
echo ""
echo "Restart Claude Code to activate all plugins."
echo ""
