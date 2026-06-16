---
name: retro
description: Post-session retrospective — scan transcripts for mistakes, corrections, missed instructions; improve rules, memories, skills, and itself. Invoke after a session or anytime with "/retro".
user-invocable: true
---
# retro — Session Retrospective & Self-Improvement

Post-session analysis skill. Scans all project session transcripts, existing
memories, CLAUDE.md rules, and other skills for gaps — then patches them.

Invoke: **`/retro`** or **`/retro <session-id-prefix>`** (target one session).

## Skill Version

<!-- retro:version:2 -->
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
| `cost_concern` | cheaper, waste tokens, subagent model, too expensive | User flagging token/cost waste — use cheaper models, fewer calls |
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
5. **Evaluate recently created memories**: Check memories created in the
   session(s) being analyzed. Are they well-formed? Do they follow the
   memory format (frontmatter + Why + How to apply)? Are they too specific
   (ephemeral task detail) or too vague (not actionable)?

Produce a memory audit report:
```
STALE: [filename] — reason
REDUNDANT: [file1] + [file2] — overlap
GAP: [correction pattern] — no memory covers this
OUTDATED: [filename] — conflicts with current CLAUDE.md
MALFORMED: [filename] — missing Why/How to apply/wrong type
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
- STALE: [list]
- GAPS: [list]
- REDUNDANT: [list]

### Proposed Changes
1. **New memory**: [name] — [what it captures]
2. **Update memory**: [name] — [what changes]
3. **Remove memory**: [name] — [why]
4. **New CLAUDE.md rule**: [section] — [rule text]
5. **Update CLAUDE.md rule**: [section] — [change]
6. **Update skill**: [skill name] — [what changes]
7. **Update retro skill**: [self-change description]

### Skipped (noise)
[Corrections that were false positives or already addressed]
```

**Gate**: Wait for user approval before applying changes. User may select
a subset (`apply 1,3,5`) or `apply all`.

---

## Phase 4: Apply Changes

For each approved change:

### 4a. Memory changes

- **New memory**: Write file to memory directory with proper frontmatter
  (name, description, metadata.type). Body follows the format: rule/fact,
  then `**Why:**` and `**How to apply:**` lines. Add entry to MEMORY.md.
- **Update memory**: Edit existing file. Update description if needed.
- **Remove memory**: Delete file and remove MEMORY.md entry.
- **Stale memory**: Update or remove based on current state.

### 4b. CLAUDE.md changes

- Read the section to edit.
- Apply minimal, targeted edits.
- Keep existing style and structure.

### 4c. Skill changes

- Read the skill file to edit.
- Apply targeted improvements.
- For self-updates: increment the version counter in this file.

### 4d. Commit

All changes in a single commit:

```bash
git add -A .claude/skills/ CLAUDE.md
git add -A /home/vscode/.claude/projects/-workspace/memory/
git commit -m "chore(retro): apply session retrospective findings

- [summary of what changed]
- retro skill v[N] → v[N+1] (if self-updated)

[ci skip]"
```

Key commit rules:
- **Always include `[ci skip]`** in the commit message — retro changes are
  rules/memory/skills only, never code that needs a pipeline.
- No AI attribution (per project rules).
- Conventional commit format: `chore(retro): <description>`.

---

## Phase 5: Verify

After committing:

1. Confirm all memory files parse correctly (valid frontmatter).
2. Confirm MEMORY.md index matches actual files.
3. Confirm CLAUDE.md has no broken markdown.
4. Confirm skill files have valid frontmatter.
5. Report summary of changes applied.

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
    /\bdo not forget\b/i, /\bensure that\b.*\bshould\b/i
  ],
  cost_concern: [
    /\bcheaper\b/i, /\bwaste.*token/i, /\bsubagent.*model\b/i,
    /\btoo.*expensive\b/i
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
3. Always increment the version counter.
4. Log what changed in the commit message body.

---

## TodoWrite Checklist

Create on invocation:

1. Scan session transcripts — extract corrections + errors
2. Audit existing memories — staleness, gaps, redundancy, accuracy
3. Analyze CLAUDE.md rules — gaps, weak rules
4. Analyze skills — workflow gaps, missed gates
5. Self-analyze — pattern gaps, false positives
6. Present findings report — get user approval
7. Apply approved changes — memories, rules, skills
8. Commit with `[ci skip]`
9. Verify — frontmatter, index, markdown
