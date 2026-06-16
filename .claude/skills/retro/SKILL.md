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
2. **Scan the CURRENT session by default — one session per invocation.** Run
   this right after a session completes: capture that session's corrections,
   apply them to CLAUDE.md, improve this skill, commit. Re-invoke `/retro`
   fresh next time a session ends. The `--all` historical sweep (one session
   at a time, with a mandatory second pass) is an opt-in backfill for building
   the initial rule set — not the normal mode. See "Iteration Protocol".

## Skill Version

<!-- retro:version:13 -->
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

**Default = the current session only.** This skill is meant to run right after
a session completes: scan THAT session's corrections and fold them into
CLAUDE.md. Do NOT batch-scan the whole transcript history by default.

- No argument → analyze **only the current session** (the conversation that
  just finished). Use the live conversation's corrections directly, or
  context-mode auto-memory search with `sort: "timeline"`.
- Session-id prefix → analyze that one historical session.
- `--all` → opt-in full historical sweep (one session at a time; rarely
  needed — only for a first-time backfill of the initial rule set).

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
| `missed_instruction` | missed, forgot, didn't, should have, "have you updated/checked?" | Failed to follow an explicit instruction or missed a proactive step |
| `premature_action` | do not start, analyze only, just plan, prepare but don't | Jumped from analysis/planning to implementation without go |
| `unrequested_addition` | why is this added, should not have been added, not asked for | Added CI jobs, features, or changes the user didn't request |
| `cost_concern` | cheaper, waste tokens, subagent model, too expensive | User flagging token/cost waste — use cheaper models, fewer calls |
| `pipeline_abort` | if not green stop/abort, if fail diagnose, abort release | Pipeline went red and assistant continued instead of stopping to diagnose |
| `positive_signal` | yes exactly, perfect, good, correct, nice | Approach was validated — preserve what worked |
| `preference_signal` | might be better, prefer to, should we, what if we, let's try | User suggesting an improvement or expressing a preference — validated when assistant agrees |
| `frustration` | idiot, moron, stupid, wtf, ffs, profanity | Strong negative signal — the preceding action was seriously wrong; always pair with another category from context |

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

## Iteration Protocol

### Normal mode — current session only (the standing rule)

When invoked after a session completes (or on demand), scan **only that one
session**: extract its corrections, fold them into CLAUDE.md, improve this
skill if a pattern was missed, commit. Re-invoke `/retro` fresh the next time
a session ends — one invocation = one session, loaded fresh from disk so the
just-improved version reads the next session.

This is the skill's permanent behavior. Do NOT scan historical sessions in
normal mode.

**Re-invokable within the same session — idempotent.** The skill can be run
many times in one session (after each milestone, not just at the very end).
Every run MUST dedup against the current CLAUDE.md and apply only findings not
already captured, so re-running never duplicates a rule. A run that finds
nothing new makes no changes and no commit — it just reports "0 new findings."
Each invocation re-reads CLAUDE.md fresh and picks up corrections that arrived
since the last run.

### One-time backfill — `--all` (not a recurring rule)

The full historical sweep exists ONLY to seed a rich initial CLAUDE.md the
first time the skill is set up. It is a one-off, not a standing behavior:

1. List sessions chronologically (oldest first).
2. Scan all of them, extract every distinct durable lesson.
3. Fold each NEW lesson (not already in CLAUDE.md) into the right section.
4. Commit. Done — the backfill is complete and not repeated.

After backfill, only the normal current-session mode runs going forward.

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
    /\bnot.*respect.*what I\b/i,
    /\bjust\s+continue\b.*\b(on|this|one)\b/i, /\bONLY\s+#?\d/i
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
    /\bensure.*all\b/i, /\bcover all\b/i, /\bat least cover\b/i,
    /\bhave you (updated|checked|done|added|posted)\b/i,
    /\bdid you (update|check|do|add|post)\b/i
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
  ],
  preference_signal: [
    /\bmight be better\b/i, /\bprefer\b.*\b(to|if|that)\b/i,
    /\bshould we\b/i, /\bwhat if we\b/i, /\bwhat about\b/i,
    /\blet'?s\s+(try|do|use|go with)\b/i, /\bcan we\b.*\binstead\b/i
  ],
  frustration: [
    /\bidiot\b/i, /\bmoron\b/i, /\bstupid\b/i, /\bdumb\b/i,
    /\bwtf\b/i, /\bffs\b/i, /\bfor f.ck.? sake\b/i,
    /\bf.cking\b/i, /\bgod ?damn\b/i
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
- "do not X yet" / "do not push/start/create" without frustration markers
  ("I said", "already told", "again", "!") — proactive workflow gate from
  user, not a correction of something the assistant already did wrong

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

Create on invocation (normal mode = current session only):

1. Scan the CURRENT session's transcript — extract corrections + errors
2. Audit auto-memories — migrate durable rules to CLAUDE.md, delete the rest
3. Analyze CLAUDE.md rules — gaps, weak rules
4. Analyze skills — workflow gaps, missed gates
5. Self-analyze — pattern gaps, false positives
6. Present findings report — get user approval
7. Apply approved changes — CLAUDE.md rules + skill improvements (NO memory creation)
8. Commit with `[ci skip]` (CLAUDE.md + skills only; memory ops are not git)
9. Verify — empty memory dir, rules landed, valid markdown, version bumped

(`--all` backfill only: loop the above over every session once, then stop.)
