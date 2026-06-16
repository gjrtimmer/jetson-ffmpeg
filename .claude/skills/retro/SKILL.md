---
name: retro
description: Post-session retrospective — scan transcripts for mistakes, corrections, missed instructions; improve rules, memories, skills, and itself. Invoke after a session or anytime with "/retro".
user-invocable: true
---
# retro — Session Retrospective & Self-Improvement

Post-session analysis skill. Scans project session transcripts, **audits**
existing auto-memories, reads CLAUDE.md rules and other skills for gaps — then
records findings **as rules in `/workspace/CLAUDE.md`**.

Invoke: **`/retro`** or **`/retro <session-id-prefix>`** (target one session).

## Two hard rules

1. **CLAUDE.md is the only destination for durable rules.** This skill does
   NOT create auto-memory files. The auto-memory directory is audited (read +
   migrate-to-CLAUDE.md + delete), never written to as a fact store. Every
   learning, preference, or rule lands in `/workspace/CLAUDE.md`.
2. **Process ONE session at a time, in chronological order.** After each
   session: apply its findings to CLAUDE.md, improve this skill (patterns,
   filters, categories), commit, THEN move to the next session. The skill must
   get smarter between sessions — never batch multiple sessions into one pass.
   When the run finishes all sessions, do a **second full pass** (see
   "Iteration Protocol") so the matured skill re-examines early sessions it was
   too naive to read correctly the first time.

## Skill Version

<!-- retro:version:8 -->
Track version here. Each self-improvement pass increments this counter and
logs what changed in the commit message.

## Token Optimization Rules

- Use `ctx_execute` / `ctx_execute_file` for all transcript analysis — raw
  JSONL bytes never enter main context.
- Use `ctx_search` for recall of already-indexed findings.
- Only `Read` files you intend to `Edit`.
- Subagents use **sonnet** unless analysis needs higher reasoning.

---

## Phase 0: Gather Session Data

### 0a. Determine scope

- No argument → analyze ALL sessions in the project workspace.
- Session-id prefix → filter to matching JSONL files.
- `--current` → analyze only the current session (use context-mode
  auto-memory search with `sort: "timeline"`).

### 0b. Extract correction signals

Run `ctx_execute` with JavaScript to scan session JSONL files. Extract user
messages matching correction/feedback patterns. The script must:

1. Read all `.jsonl` files from the project workspace directory
   (`/home/vscode/.claude/projects/-workspace/`).
2. Parse each line; for `type: "user"` entries, extract text from
   `message.content[].text`.
3. Strip IDE context tags (`<ide_*>`, `<task-notification>`, etc.).
4. Classify each correction into categories:

| Category | Patterns | What it means |
|----------|----------|---------------|
| `workflow_order` | wait, stop, not yet, hold, do not X yet | Assistant acted before user was ready or skipped a gate |
| `tool_misuse` | instead, should use, wrong tool | Wrong tool or approach chosen |
| `scope_drift` | not what I said, I said, I didn't ask, not what I asked | Assistant did something different from what was requested |
| `rule_violation` | never, already told you, we agreed, don't again | Violated an established rule or repeated a known mistake |
| `missed_instruction` | missed, forgot, didn't, should have | Failed to follow an explicit instruction |
| `premature_action` | do not start, analyze only, just plan, prepare but don't | Jumped from analysis/planning to implementation without go |
| `unrequested_addition` | why is this added, should not have been added, not asked for | Added CI jobs, features, or changes the user didn't request |
| `cost_concern` | cheaper, waste tokens, subagent model, too expensive | User flagging token/cost waste — use cheaper models, fewer calls |
| `pipeline_abort` | if not green stop/abort, if fail diagnose, abort release | Pipeline went red and assistant continued instead of stopping to diagnose |
| `positive_signal` | yes exactly, perfect, good, correct, nice | Approach was validated — preserve what worked |

5. For each correction, also extract the **preceding assistant message** (the
   thing the user corrected) to understand what went wrong.
6. Return a structured summary: `{category, userMsg, assistantContext, sessionId, timestamp}`.

### 0c. Extract assistant errors

Also scan for assistant-side signals:

- Tool calls that returned errors (look for `type: "assistant"` entries with
  tool results containing error strings).
- Repeated tool calls (same tool + same args within 3 turns = thrashing).
- Tasks that took >10 turns to complete (potential inefficiency).

---

## Phase 1: Analyze Existing Memories

Read all memory files from `/home/vscode/.claude/projects/-workspace/memory/`.

For each memory file:

1. **Staleness check**: Does the memory reference files, functions, or flags
   that still exist? Use `grep` / `semble search` to verify. If the
   referenced artifact is gone, mark memory as stale.
2. **Redundancy check**: Are two memories saying the same thing? Flag
   duplicates.
3. **Coverage check**: Compare corrections found in Phase 0 against existing
   memories. Are there recurring corrections with no corresponding memory?
   These are gaps.
4. **Accuracy check**: Does the memory match current CLAUDE.md rules? If a
   rule was added to CLAUDE.md but the memory still says the old behavior,
   the memory is outdated.
5. **Migrate to CLAUDE.md**: Any memory file that still carries a durable rule
   or preference NOT yet in CLAUDE.md is migrated — fold its content into the
   correct CLAUDE.md section, then delete the memory file. The auto-memory
   directory must end empty of fact files (only `MEMORY.md` remains, noting
   that CLAUDE.md is the source of truth). **Never create new memory files.**

Produce a memory audit report:
```
MIGRATE: [filename] — rule to fold into CLAUDE.md §<section>
STALE: [filename] — closed issue / dead reference → delete
REDUNDANT: [filename] — already in CLAUDE.md → delete
KEEP: MEMORY.md — pointer to CLAUDE.md only
```

---

## Phase 2: Analyze Rules & Skills

### 2a. CLAUDE.md gap analysis

Compare Phase 0 corrections against CLAUDE.md sections. For each correction
category with 2+ occurrences and no matching CLAUDE.md rule:

- Draft a new rule (imperative, concise, with rationale).
- Identify where in CLAUDE.md it belongs (which section).

For corrections that DO have a matching rule but keep recurring:

- The rule isn't prominent enough or isn't specific enough.
- Draft a strengthened version with explicit examples.

### 2b. Skill gap analysis

For each project skill (`.claude/skills/*/SKILL.md`):

- Are there corrections related to this skill's workflow? (e.g., fix-issue
  gate violations → fix-issue skill needs stronger gate language)
- Are there workflow patterns the user repeatedly corrects that a skill
  should enforce?

### 2c. Self-analysis (this skill)

Compare the retro skill's detection patterns against what it actually found
vs. what it missed. If corrections were manually identified that the Phase 0
patterns didn't catch:

- Add new patterns to the classification table.
- Refine existing patterns that produced false positives.
- Update the skill version counter.

---

## Phase 3: Present Findings

**Always present findings to the user before making any changes.** Format:

```markdown
## Retro Report — [date] ([N] sessions analyzed)

### Corrections Found
| # | Category | Session | User said | What went wrong |
|---|----------|---------|-----------|-----------------|
| 1 | scope_drift | 4e1f... | "I did not say put resolution..." | Put wrong content in issue comment |

### Memory Audit
- MIGRATE: [list — memory → CLAUDE.md section]
- STALE/REDUNDANT (delete): [list]

### Proposed Changes
1. **New CLAUDE.md rule**: [section] — [rule text]
2. **Update CLAUDE.md rule**: [section] — [strengthened text]
3. **Migrate memory → CLAUDE.md**: [filename] → [section], then delete file
4. **Delete stale memory**: [filename] — [why]
5. **Update skill**: [skill name] — [what changes]
6. **Update retro skill**: [self-change description]

> There is no "new memory" option — findings become CLAUDE.md rules.

### Skipped (noise)
[Corrections that were false positives or already addressed]
```

**Gate**: Wait for user approval before applying changes. User may select
a subset (`apply 1,3,5`) or `apply all`.

---

## Phase 4: Apply Changes

For each approved change:

### 4a. CLAUDE.md changes (the destination for ALL findings)

- Read the target section.
- Add new rules / strengthen existing ones with minimal, targeted edits.
- Match existing style: imperative, concise, with a short rationale.
- Pick the right section (Working agreements, Commit conventions, Build &
  test, Issue workflow, etc.) — don't invent a new section unless none fits.

### 4b. Memory audit actions (read + migrate + delete — NEVER create)

- **Migrate**: fold the memory's durable content into the correct CLAUDE.md
  section, then `rm` the memory file.
- **Delete stale/redundant**: `rm` the file (it's a closed issue or already
  in CLAUDE.md).
- Leave `MEMORY.md` as a pointer that says CLAUDE.md is the source of truth.
- **Do not write new memory fact files. Ever.**

### 4c. Skill changes

- Read the skill file to edit.
- Apply targeted improvements.
- For self-updates: increment the version counter in this file.

### 4d. Commit (one commit per session in iterative mode)

```bash
git add -A .claude/skills/ CLAUDE.md
git commit -m "chore(retro): <session-id> findings, skill vN→vN+1

- [findings applied to CLAUDE.md]
- [pattern/filter/category improvements]

[ci skip]"
```

Note: auto-memory files live OUTSIDE the repo
(`/home/vscode/.claude/projects/-workspace/memory/`) and are not git-tracked —
deleting/migrating them is a filesystem op, not a commit.

Key commit rules:
- **Always include `[ci skip]`** — retro changes are rules/skills only, never
  code that needs a pipeline. (Note: this repo's CI also honors `-o ci.skip`
  on push; the `[ci skip]` in the message is the in-commit equivalent.)
- No AI attribution (per project rules).
- Conventional commit format: `chore(retro): <description>`.

---

## Phase 5: Verify

After committing:

1. Confirm the auto-memory directory holds no fact files (only `MEMORY.md`).
2. Confirm every migrated rule actually landed in CLAUDE.md.
3. Confirm CLAUDE.md has no broken markdown.
4. Confirm the skill file has valid frontmatter and a bumped version counter.
5. Report summary of changes applied for this session.

---

## Iteration Protocol (per-session + second pass)

This skill improves itself as it works, so the ORDER and GRANULARITY matter:

### One session at a time

1. List sessions chronologically (oldest first).
2. For the next unprocessed session: run Phases 0-5 on **that session only**.
3. Apply findings to CLAUDE.md, improve this skill, commit. The skill is now
   smarter.
4. Move to the next session. Repeat.

Never analyze several sessions before applying — a naive early version of the
skill would read all of them with the same blind spots. Improving between
sessions means session N+1 is read by a better skill than session N was.

**Re-invoke the skill (fresh `Skill` tool call) for EACH session.** After a
session's findings are applied and committed, the skill file on disk has
changed (new patterns/filters/categories). Do NOT keep scanning with the
skill text already loaded in context — that is the stale, pre-improvement
version. Re-invoke `/retro` so the next session is read by the just-improved
skill loaded fresh from disk. One invocation = one session.

### Second pass (mandatory after first pass completes)

Once every session has been processed once, the skill is at its most mature.
Early sessions were read by immature versions that missed things. So:

1. Re-run the full chronological sweep, one session at a time, with the now-
   matured skill.
2. Only NEW findings (not already in CLAUDE.md) produce changes.
3. Stop when a full pass yields zero new findings (convergence). In practice
   two passes suffice; a third is only needed if pass 2 changed the patterns
   materially.

Log each pass: `pass N — session <id> — X new findings`.

---

## Detection Pattern Reference

These are the regex patterns used to classify user corrections. Phase 2c
self-improvement may add/refine entries.

<!-- retro:patterns:start -->
```javascript
const PATTERNS = {
  workflow_order: [
    /\bwait\s+(for|till|until)\b/i, /\bstop\b/i, /\bnot yet\b/i,
    /\bhold\b/i, /\bdo not.*yet\b/i, /\bbefore I\b/i, /\btill I\b/i,
    /\blet me\b.*\bfirst\b/i, /\bpause\b/i,
    /\bdo not\s+(start|create|push|merge)\b/i
  ],
  tool_misuse: [
    /\byou\b.*\binstead\b/i, /\bshould.*use\b/i, /\bwrong\b/i,
    /\bdon'?t use\b/i, /\bnot\s+correct\b/i,
    /\bdoes not seem\b/i, /\bnot.*right\b/i
  ],
  scope_drift: [
    /\bnot what I\b/i, /\bI said\b/i, /\bI didn'?t say\b/i,
    /\bI asked\b/i, /\bnot what I asked\b/i, /\bthat'?s not what\b/i,
    /\bI did not say\b/i, /\bI meant\b/i, /\byou added.*not.*ask/i,
    /\bnot.*respect.*what I\b/i
  ],
  rule_violation: [
    /\balready told\b/i, /\bwe agreed\b/i,
    /\bwe said\b/i, /\bdon'?t.*again\b/i,
    /\bmake this a rule\b/i, /\byou include.*auto.*generat/i
  ],
  missed_instruction: [
    /\bforgot\b/i, /\bshould have\b/i,
    /\byou need to\b/i, /\bwas supposed to\b/i,
    /\bdo not forget\b/i, /\bensure that\b.*\bshould\b/i,
    /\bensure.*all\b/i, /\bcover all\b/i, /\bat least cover\b/i
  ],
  unrequested_addition: [
    /\bwhy\s+(is|was)\s+this\s+added\b/i, /\bshould not have been added\b/i,
    /\bnot\s+asked\s+for\b/i, /\bwho\s+asked\b/i,
    /\bI\s+didn'?t\s+(ask|request)\b/i
  ],
  premature_action: [
    /\bdo not\s+start\b/i, /\bjust\s+(analyze|plan|check)\b/i,
    /\bprepare.*don'?t\b/i, /\banalyze.*not.*implement\b/i,
    /\bplan.*wait\b/i, /\bpresent.*wait\b/i
  ],
  cost_concern: [
    /\bcheaper\b/i, /\bwaste.*token/i, /\bsubagent.*model\b/i,
    /\btoo.*expensive\b/i
  ],
  pipeline_abort: [
    /\bif.*not.*green.*(stop|abort)\b/i, /\bstop and abort\b/i,
    /\bif\b.*\bfail\b.*\b(stop|diagnose|abort)\b/i,
    /\babort\b.*\b(release|tag|pipeline)\b/i
  ],
  positive_signal: [
    /\byes exactly\b/i, /\bperfect\b/i, /\bgood\b.*\bapproach\b/i,
    /\bcorrect\b/i, /\bthat'?s right\b/i,
    /\byes,?\s*(go|do|proceed|continue)\b/i, /\bapproved\b/i
  ]
};
```
<!-- retro:patterns:end -->

---

## Noise Filters

Skip messages that match correction patterns but are NOT user feedback:

- Messages inside `<task-notification>` or `<local-command-caveat>` tags
- Messages that are quoting external content (issue bodies, PR comments)
- Messages shorter than 10 chars after tag stripping
- Messages that are just "yes" / "go" / "continue" (approval, not correction)
- Messages referencing upstream/fork issues (user describing external bugs,
  not correcting the assistant)
- Session resume context ("This session is being continued from a previous")
- Skill loading preambles ("Base directory for this skill:")
- Code-context "wait" (e.g. `void(wait(`) — not a workflow gate
- Terminal output pastes (lines starting with `$` or `jetson:`)
- "I will wait" — user declaring patience, not a correction
- "waiting for X to be merged" without "wait for me/till/until" — context
- Bare `\bnever\b` in instructional context (CLAUDE.md rules inject "never"
  into session context, producing false positives; require `already told` or
  `make this a rule` instead)
- Bare `\bmissed\b` or `\bdidn't\b` without subject "you" nearby (user
  describing external bugs, not correcting assistant)

---

## Self-Improvement Protocol

When this skill updates itself (Phase 2c):

1. Changes are scoped to:
   - Detection patterns (add/refine/remove regex in the pattern reference)
   - Noise filters (add new exclusion rules)
   - Phase instructions (clarify ambiguous steps that led to wrong analysis)
   - Category definitions (add new categories if recurring corrections don't
     fit existing ones)
2. Never remove core phases (0-5) or the gating requirement (Phase 3).
3. Never reintroduce memory-file creation — CLAUDE.md is the only destination.
4. Always increment the version counter.
5. Log what changed in the commit message body.

---

## TodoWrite Checklist

Create on invocation (per session, in chronological order):

1. Scan ONE session's transcript — extract corrections + errors
2. Audit auto-memories — migrate durable rules to CLAUDE.md, delete the rest
3. Analyze CLAUDE.md rules — gaps, weak rules
4. Analyze skills — workflow gaps, missed gates
5. Self-analyze — pattern gaps, false positives
6. Present findings report — get user approval
7. Apply approved changes — CLAUDE.md rules + skill improvements (NO memory creation)
8. Commit with `[ci skip]` (CLAUDE.md + skills only; memory ops are not git)
9. Verify — empty memory dir, rules landed, valid markdown, version bumped
10. Move to next session; after all sessions, run the mandatory second pass
