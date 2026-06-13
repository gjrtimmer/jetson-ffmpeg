---
name: fix-issue
description: End-to-end workflow for fixing a GitHub issue — triage, plan, implement, test, ship. Invoke with "Fix Issue #NR".
user-invocable: true
---
# fix-issue

Fix a GitHub issue end-to-end. Invoke: **"Fix Issue #NR"**.

## Token Optimization Rules

These rules are MANDATORY at every phase. Violating them wastes context budget.

### Never read raw output into main context

| Want to... | Use this | NOT this |
|------------|----------|----------|
| Run commands, see results | `ctx_batch_execute` | Bash (unless mutating state) |
| Search indexed results | `ctx_search` | Re-reading files |
| Analyze/filter/count data | `ctx_execute` or `ctx_execute_file` | Reading + reasoning in context |
| Find code semantically | `mcp__semble__search` | Grep + Read |
| Read file to analyze | `ctx_execute_file` | Read (unless editing) |
| Read file to edit | Read | ctx_execute_file |

### Subagent rules

- **Default model: sonnet** for all subagents unless task needs higher reasoning.
- Escalation ladder: sonnet → opus 4.6 → latest opus → fable (last resort).
- Subagents MUST return structured findings only — file:line references, not file contents.
- Use `subagent_type: "Explore"` or `subagent_type: "semble-search"` for investigation.
- Use `subagent_type: "caveman:cavecrew-builder"` for 1-2 file surgical edits.
- Use `subagent_type: "caveman:cavecrew-investigator"` for read-only code location.
- Main context is the orchestrator — it delegates, synthesizes, and decides. It does NOT do bulk reading.

### Phase gates

Each phase produces a compact artifact (summary, plan, result). Only that artifact
carries forward. Raw investigation data stays in subagents or ctx indexes.

### Issue communication

**Every phase posts a status comment on the GitHub issue.** This creates a
public audit trail and keeps watchers informed. Use `gh issue comment NR -R
gjrtimmer/jetson-ffmpeg --body "..."` at each gate.

| Phase | Comment content |
|-------|----------------|
| 0 Triage | "Triaging — confirmed actionable, no existing PR/branch" |
| 1 Plan | Full implementation plan (root cause, files, tests, risks) |
| 2 Implement | "Implementation started on branch `fix/NR-short-desc`" |
| 3 Local verify | "Local verification: build OK, hw-all [PASS/FAIL details]" |
| 4 Full matrix | "smoke-all.sh: [N/7] green [details if partial]" |
| 5 Ship | Full resolution comment (commits, files, root cause, validation) |
| 6 Upstream | Comment on upstream issues (if applicable) |

Failure comments are equally important — if a phase fails and needs rework,
post what failed and what the fix approach is. Silence on an issue = abandoned work.

---

## Phase 0: Triage

Fetch issue + check for prior work. Use `ctx_batch_execute` for all commands:

```javascript
commands: [
  { label: "issue-details", command: "gh issue view NR -R gjrtimmer/jetson-ffmpeg --json number,title,body,labels,comments,assignees" },
  { label: "existing-prs", command: "gh pr list -R gjrtimmer/jetson-ffmpeg --search '#NR' --state open --json number,url,headRefName" },
  { label: "existing-branches", command: "git branch -a | grep -i 'NR'" }
]
queries: ["issue title", "issue body", "labels", "existing PR"]
```

**Gate**: If PR exists or issue already fixed → STOP and report. Note any
upstream references (Keylost#XX, jocover#XX) for Phase 6.

## Phase 1: Investigate & Plan

### 1a. Investigation (subagent — sonnet, Explore type)

Spawn investigation subagent with:
- Issue title + body (from Phase 0 artifact)
- Label-derived search areas (area:decoder → `src/nvmpi_dec.cpp`, etc.)
- Task: find root cause, list affected files:lines, related tests

Subagent uses semble search + ctx tools internally. Returns ONLY:
```
Root cause: <1-2 sentences>
Files: <path:line list>
Tests: <existing test files>
New test needed: <yes/no + what>
Risk: <what could break>
```

If sonnet can't determine root cause → re-run at opus 4.6.

### 1b. Plan & post

Synthesize subagent findings into plan. Post on issue:
```bash
gh issue comment NR -R gjrtimmer/jetson-ffmpeg --body "<plan>"
```

Plan covers: root cause, files to change, test strategy, risks.

**Gate**: Ask user for "go". A "go" approves the presented plan. If execution
reveals material scope change → stop and re-confirm.

## Phase 2: Branch & Implement

### Branch naming

Format: `fix/{NR}-{short-desc}` — kebab-case from issue title, max 40 chars.
```bash
git checkout -b fix/NR-short-desc main
```

### Status update

Post comment on issue:
```bash
gh issue comment NR -R gjrtimmer/jetson-ffmpeg --body "Work started on branch \`fix/NR-short-desc\`."
```

### Implementation

- Minimal change. No scope creep.
- Only Read files you're about to Edit (not for analysis — use ctx tools).
- If touching `ffmpeg/dev/common/` → run `./ffmpeg/dev/update_patch.sh` after.
- Never hand-edit `ffmpeg/patches/*.patch`.
- For multi-file changes: use `caveman:cavecrew-builder` subagents (sonnet) for
  independent edits in parallel.

### Test writing

- Add/update `test/hw-*.sh` suites following existing patterns.
- Read `test/README.md` for conventions (only if writing new suite).

## Phase 3: Local Verification

Run on Jetson hardware (devcontainer has `--runtime=nvidia`, Orin GPU live):

```bash
./scripts/build.sh --install
JETSON_VARIANT=orin-nano ./test/hw-all.sh
```

Use `ctx_batch_execute` for build output analysis. Use `ctx_execute` to parse
test failures if output is large.

**Loop**: fix → build → test → repeat until green. Do NOT proceed with failures.

### Status update

Post comment on issue with local test results:
```bash
gh issue comment NR -R gjrtimmer/jetson-ffmpeg --body "Local verification: build OK, hw-all PASS (all suites green on Orin)."
```
On failure, post what failed and the fix approach before iterating.

## Phase 4: Full Matrix

```bash
./test/smoke-all.sh
```

7 FFmpeg versions, ~30 min. If partial failure:
- Use `ctx_search` on indexed output to find which version failed
- Fix version-specific issue
- Re-run with `-v "X.Y"` subset first, then full

**Gate**: 7/7 green required. Never commit with failures.

### Status update

Post comment on issue with matrix results:
```bash
gh issue comment NR -R gjrtimmer/jetson-ffmpeg --body "smoke-all.sh: 7/7 green (FFmpeg 4.2, 4.4, 6.0, 6.1, 7.0, 7.1, 8.0)."
```

## Phase 5: Commit & Ship

1. **Commit** — conventional format, `Fixes #NR` footer:
   ```bash
   git add <specific files>
   git commit -m "fix(scope): description

   Fixes #NR"
   ```

2. **Evidence comment on issue**:
   ```bash
   gh issue comment NR -R gjrtimmer/jetson-ffmpeg --body "Resolution: ..."
   ```
   Include: commits, files changed, root cause, fix summary, validation results.

3. **Push + PR**:
   ```bash
   git push -u origin fix/NR-short-desc
   gh pr create -R gjrtimmer/jetson-ffmpeg --title "fix(scope): desc" --body "..."
   ```

## Phase 6: Upstream Notification

If issue references open upstream issues (Keylost#XX, jocover#XX), post brief
factual comment with commit link and test evidence. Skip closed upstream issues.

## TodoWrite Checklist

Create on invocation:
1. Triage issue #NR — post triage status on issue
2. Investigate root cause (subagent)
3. Post implementation plan on issue, get user approval
4. Create branch `fix/NR-short-desc` from main
5. Post "work started" comment on issue
6. Implement fix
7. Write/update tests
8. Local verify — build + hw-all, post results on issue
9. Full matrix — smoke-all.sh 7/7, post results on issue
10. Commit with `Fixes #NR` footer
11. Post resolution/evidence comment on issue
12. Push branch + create PR
13. Upstream notification (if applicable)
