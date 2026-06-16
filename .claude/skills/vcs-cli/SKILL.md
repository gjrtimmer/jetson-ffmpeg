---
name: vcs-cli
description: Verified gh (GitHub) and glab (GitLab) command cheatsheet for this repo — common ops + correct optional flags (ci.skip, immediate merge, auto-merge). Use to avoid rediscovering flags and burning tokens on --help.
user-invocable: true
---
# vcs-cli — gh + glab Command Cheatsheet

Token-saving reference. **Use these verified commands instead of running
`--help` mid-task.** Flags here are confirmed against the installed `glab`/`gh`
in this devcontainer. If a flag fails, the tool version changed — re-verify
with `--help` and update this skill (it's the same self-improvement contract
as the `retro` skill: fix the doc when reality drifts).

Project context:
- GitLab: `gitlab.timmertech.nl`, project `timmertech/jetson-ffmpeg` (active CI).
- GitHub: `gjrtimmer/jetson-ffmpeg` (push-mirror of GitLab; issues live here).
- Always `glab auth status` / `gh auth status` before remote ops. If not
  authenticated, STOP and ask the user to `glab auth login` / `gh auth login`.

---

## Skipping CI / pipelines (the important part)

**There is NO `--skip-ci` flag on `glab mr merge`.** Pipeline skipping happens
at **push/commit time**, not merge time. Three mechanisms:

| Goal | Mechanism |
|------|-----------|
| No pipeline on a push | `git push -o ci.skip` (push option) |
| No pipeline for a commit | `[ci skip]` or `[skip ci]` in the commit message |
| Skip branch pipeline when opening an MR right after | `git push -u origin <branch> -o ci.skip` then `glab mr create …` (the MR pipeline is the one that matters) |

For a **docs-only** branch where the head commit already carries `[ci skip]`,
GitLab creates no pipeline at all. With `only_allow_merge_if_pipeline_succeeds`
set, "no pipeline" means there's nothing to block the merge.

---

## glab — merge requests

```bash
# View MR (machine-readable — pipe to python/jq, never eyeball huge JSON)
glab mr view <iid> --output json
glab mr list --all                       # list (shows source→target branches)

# Create MR → main, non-interactive
glab mr create --fill --yes \
  --source-branch <branch> --target-branch main \
  --title "<type>(<scope>): <subject>" --description "<body>"
#   --fill   = infer title/body from commits      --yes = no prompt
#   --draft  = open as draft        -a/--assignee, -l/--label, --reviewer

# Merge — AUTO-MERGE is the DEFAULT when a pipeline is running
glab mr merge <iid> --yes                        # auto-merge (waits for pipeline)
glab mr merge <iid> --auto-merge=false --yes     # MERGE IMMEDIATELY (no wait)
#   -s/--squash   -r/--rebase   -d/--remove-source-branch
#   -m "<msg>"    --sha <sha> (merge only if HEAD matches)
```

**Merge decision rules (this repo, from CLAUDE.md):**
- Normal: `glab mr merge <iid> --yes` (auto-merge; waits for pipeline to pass).
- **Wait for the pipeline to EXIST before auto-merge** — setting auto-merge
  before a pipeline is created returns HTTP 405. Poll
  `glab mr view <iid> --output json` until `head_pipeline` is non-null
  (or `sleep 15`), then enable.
- Immediate merge (`--auto-merge=false`) is an override — only when explicitly
  approved (e.g. one-time docs-only merge with pipeline skipped at push).

```bash
# Pipelines
glab ci status                                   # current branch pipeline
glab ci view <pipeline-id>
glab ci lint                                     # validate .gitlab-ci.yml (live)
```

`glab ci lint` after EVERY `.gitlab-ci.yml` edit — resolves anchors/extends/
rules against the live instance (a plain YAML parse cannot).

---

## gh — issues (repo: gjrtimmer/jetson-ffmpeg)

```bash
gh issue list   -R gjrtimmer/jetson-ffmpeg --state open
gh issue view <n> -R gjrtimmer/jetson-ffmpeg --json number,title,body,labels,comments,state
gh issue create  -R gjrtimmer/jetson-ffmpeg --title "…" --body "…"
gh issue comment <n> -R gjrtimmer/jetson-ffmpeg --body "…"
gh issue view <n> -R owner/repo --json state -q '.state'   # check open before upstream-notify
```

- Always pass `-R gjrtimmer/jetson-ffmpeg` — don't guess the slug.
- Close code-fixed issues via `Closes #N`/`Fixes #N` commit footers only,
  never by hand. Always add an evidence comment.
- `--json <fields> -q '<jq>'` keeps output tiny (token-saving).

## gh — PRs (mirror; GitLab MR is primary)

```bash
gh pr list   -R gjrtimmer/jetson-ffmpeg --state open --json number,title,headRefName
gh pr view <n> -R gjrtimmer/jetson-ffmpeg --json url,comments,reviews
gh pr create -R gjrtimmer/jetson-ffmpeg --title "…" --body "…"   # fallback only
```

---

## Token-saving conventions

- **Always `--output json` (glab) / `--json … -q` (gh)** and pipe through
  `python3`/`jq` to extract only the fields you need — never dump full objects
  into context.
- For "wait until merged/green", use a **background** poll
  (`run_in_background`) with a `sleep 300` loop that exits on the terminal
  state — it re-invokes only on completion, costing zero interim tokens.
- Don't run `<cmd> --help` mid-task — consult this skill. If a flag is missing
  here, verify once and add it.

---

## This repo's push → MR → merge flow (quick reference)

```bash
# Code branch (gated): smoke-all green → /retro → push
./test/smoke-all.sh                              # 7/7 required
git push -u origin <branch>                      # branch pipeline runs
glab mr create --fill --yes --target-branch main
# wait for head_pipeline non-null, then:
glab mr merge <iid> --yes                        # auto-merge

# Docs/skill-only branch (exempt): commits carry [ci skip]
git push -u origin <branch> -o ci.skip           # no branch pipeline
glab mr create --fill --yes --target-branch main
glab mr merge <iid> --yes                         # or --auto-merge=false --yes if approved
```
