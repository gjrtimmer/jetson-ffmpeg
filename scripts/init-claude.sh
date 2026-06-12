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
NC='\033[0m'

info()  { printf '%b[OK]%b %s\n' "$GREEN" "$NC" "$1"; }
warn()  { printf '%b[!!]%b %s\n' "$YELLOW" "$NC" "$1"; }
fail()  { printf '%b[ERR]%b %s\n' "$RED" "$NC" "$1"; }
header(){ printf '\n%b=== %s ===%b\n' "$BOLD" "$1" "$NC"; }

# ---------------------------------------------------------------------------
# Prerequisites
# ---------------------------------------------------------------------------
missing=0
for cmd in node npx gh; do
    if ! command -v "$cmd" &>/dev/null; then
        fail "$cmd not found in PATH"
        missing=1
    fi
done
(( missing )) && exit 1

if [[ ! -d "${HOME}/.claude" ]]; then
    fail "$HOME/.claude not found — run 'claude' once to login first"
    exit 1
fi

# ---------------------------------------------------------------------------
# Caveman — token-efficient communication mode
# https://github.com/JuliusBrussee/caveman
# ---------------------------------------------------------------------------
header "Caveman"
if curl -fsSL https://raw.githubusercontent.com/JuliusBrussee/caveman/main/install.sh | bash; then
    info "Caveman installed"
else
    warn "Caveman install failed (non-fatal)"
fi

# ---------------------------------------------------------------------------
# Ruflo — AI agent orchestration (global npm install only, MCP in .mcp.json)
# https://github.com/ruvnet/claude-flow
# ---------------------------------------------------------------------------
header "Ruflo"
if npm install -g ruflo@latest 2>/dev/null; then
    info "Ruflo installed globally"
else
    warn "Ruflo install failed (non-fatal)"
fi

# ---------------------------------------------------------------------------
# Semble — fast code search for agents
# https://github.com/MinishLab/semble
# ---------------------------------------------------------------------------
header "Semble"
if command -v uv &>/dev/null; then
    if uv tool install semble 2>/dev/null; then
        info "Semble installed"
    else
        warn "Semble install failed (non-fatal)"
    fi
else
    warn "uv not found — skipping Semble (install uv first)"
fi

# ---------------------------------------------------------------------------
# GH Issues Auto-Fixer — automated issue-to-PR skill
# https://github.com/openclaw/openclaw (skills/gh-issues)
# ---------------------------------------------------------------------------
header "GH Issues Auto-Fixer"
if gh skill install openclaw/openclaw gh-issues 2>/dev/null; then
    info "gh-issues skill installed"
else
    warn "gh skill install failed — try: npx skills add https://github.com/openclaw/openclaw --skill gh-issues"
fi

# ---------------------------------------------------------------------------
# SuperPowers — agentic skills framework
# https://github.com/obra/superpowers
# ---------------------------------------------------------------------------
header "SuperPowers plugin"
if claude plugin install superpowers@claude-plugins-official 2>/dev/null; then
    info "SuperPowers installed"
else
    warn "SuperPowers install failed (non-fatal)"
fi

# ---------------------------------------------------------------------------
# Context Mode — context window optimization (98% reduction)
# https://github.com/mksglu/context-mode
# ---------------------------------------------------------------------------
header "Context Mode plugin"
claude plugin marketplace add mksglu/context-mode 2>/dev/null || true
if claude plugin install context-mode@context-mode 2>/dev/null; then
    info "Context Mode installed"
else
    warn "Context Mode install failed (non-fatal)"
fi

# ---------------------------------------------------------------------------
# Fullstack Dev Skills — 66 specialized dev skills
# https://github.com/Jeffallan/claude-skills
# ---------------------------------------------------------------------------
header "Fullstack Dev Skills plugin"
claude plugin marketplace add jeffallan/claude-skills 2>/dev/null || true
if claude plugin install fullstack-dev-skills@jeffallan 2>/dev/null; then
    info "Fullstack Dev Skills installed"
else
    warn "Fullstack Dev Skills install failed (non-fatal)"
fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
header "Setup Complete"
echo ""
echo "Installed:"
echo "  - Caveman        (skills)"
echo "  - Ruflo           (global npm, MCP via .mcp.json)"
echo "  - Semble          (uv tool, MCP via .mcp.json)"
echo "  - gh-issues       (gh skill)"
echo "  - SuperPowers     (plugin)"
echo "  - Context Mode    (plugin)"
echo "  - Dev Skills      (plugin)"
echo ""
echo "Restart Claude Code to activate all plugins."
echo ""
if [[ -z "${GITHUB_TOKEN:-}" ]]; then
    warn "GITHUB_TOKEN not set — GitHub MCP server will not authenticate"
    echo "  Set it on your host: export GITHUB_TOKEN=ghp_..."
    echo "  The devcontainer passes it through via \${localEnv:GITHUB_TOKEN}"
fi
echo ""
