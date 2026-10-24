---
name: create-release
description: >-
  Autonomous release workflow — waits for open MRs and pipelines to land green,
  then bumps version, regenerates changelog, tags, and pushes. Walk-away safe.
  Invoke with "/create-release [vX.Y.Z]" — version auto-detected if omitted.
argument-hint: "[vX.Y.Z | patch | minor | major]"
user-invocable: true
---
# create-release

Create a release end-to-end. Invoke: **`/create-release [vX.Y.Z]`**.

Version is optional — if omitted, auto-detected from commit history since last tag.

Walk-away safe: if MRs or pipelines are still in-flight, the skill monitors
them autonomously and proceeds only after everything lands green on main.

## Skill Activation

Before starting work, activate these skills if not already active:
- `/caveman full` — terse output, saves context budget

## Token Optimization Rules

| Want to... | Use this | NOT this |
|------------|----------|----------|
| Run commands, see results | `ctx_batch_execute` | Bash (unless mutating state) |
| Search indexed results | `ctx_search` | Re-reading files |
| Analyze/filter/count data | `ctx_execute` or `ctx_execute_file` | Reading + reasoning in context |

### Subagent rules

- **Default model: haiku** for grunt work (pipeline status, git log, grep).
- Escalate to **sonnet** for investigation or judgment calls.
- Use `subagent_type: "vcs-cli"` for `glab`/`gh` operations.

---

## Phase 0: Determine Version

Three input modes:

| Argument | Behavior |
|----------|----------|
| `v3.8.0` | Use exactly as given |
| `patch` / `minor` / `major` | Bump that component of the latest tag |
| _(none)_ | Auto-detect from commit history since last tag |

### Auto-detection (no argument)

1. Get latest tag: `git describe --tags --abbrev=0`
2. Parse into `MAJOR.MINOR.PATCH`.
3. Scan commits since that tag for Conventional Commit types:

```bash
git log --pretty="%s" $(git describe --tags --abbrev=0)..main
```

4. Apply semver rules:
   - Any commit with `!` after type/scope OR a `BREAKING CHANGE:` footer → **major** bump.
   - Any `feat` or `feat(...)` type → **minor** bump (unless major).
   - Only `fix`, `perf`, `refactor`, `docs`, `chore`, `ci`, `build`, `test`, `style` → **patch** bump.

5. Compute new version and confirm with user:
   ```
   Latest tag: v3.7.0
   Commits since tag: 4 feat, 2 fix, 1 chore
   Detected bump: minor → v3.8.0
   Proceed? [Y/n]
   ```

### Explicit version

If argument matches `vX.Y.Z` → use directly. Validate it's > latest tag.

### Bump keyword

If argument is `patch`/`minor`/`major` → get latest tag, apply bump.

### Output

```
VERSION_TAG = "vX.Y.Z"
VERSION     = "X.Y.Z"
```

---

## Phase 1: Pre-flight Checks

Run ALL checks before any mutation. Use `ctx_batch_execute`:

```javascript
commands: [
  { label: "auth-glab",     command: "glab auth status 2>&1" },
  { label: "auth-gh",       command: "gh auth status 2>&1" },
  { label: "current-branch", command: "git branch --show-current" },
  { label: "git-status",    command: "git status --porcelain" },
  { label: "main-head",     command: "git log --oneline -1 main" },
  { label: "existing-tag",  command: "git tag -l VERSION_TAG" },
  { label: "open-mrs",      command: "glab mr list --state opened 2>&1" },
  { label: "running-pipelines", command: "glab ci list --status running 2>&1 | head -20" },
  { label: "pending-pipelines", command: "glab ci list --status pending 2>&1 | head -20" },
  { label: "main-pipeline", command: "glab api 'projects/timmertech%2Fjetson-ffmpeg/pipelines?ref=main&per_page=1' 2>&1 | jq '.[0] | {id, status, sha}'" },
  { label: "current-version", command: "git describe --tags --abbrev=0 --match 'v[0-9]*'" },
  { label: "changelog-head", command: "head -5 CHANGELOG.md" }
]
queries: ["auth status", "open merge requests", "pipeline status", "existing tag"]
```

### Validation gates (all must pass)

| Check | Pass condition | Fail action |
|-------|---------------|-------------|
| glab auth | Authenticated | STOP — ask user to `glab auth login` |
| gh auth | Authenticated | STOP — ask user to `gh auth login` |
| Working tree | Clean (`git status --porcelain` empty) | STOP — ask user to commit/stash |
| Tag not taken | `git tag -l` returns empty | STOP — tag exists, ask user if retraction intended |
| Version sanity | New version > latest `v*` tag | STOP — version would be a downgrade |

### Open MRs check

If open MRs exist targeting main:
1. List them with title + pipeline status.
2. **Assume all are release-blocking.** Do NOT ask the user — open MRs
   targeting main are always intended for the release. Proceed directly
   to **Phase 2: Wait for MRs**.
3. Only skip Phase 2 if there are zero open MRs.

### Pipeline check

If the latest main pipeline is NOT `success`:
1. Report its status.
2. Proceed to **Phase 2: Wait for Pipelines**.

The main pipeline after a merge runs builds only (`HW_TIER=none`). The
**pre-tag gate** is a full-tier pipeline (`HW_TIER=full`: weekly schedule or a
web run) that is `success` on the exact `main` HEAD SHA to be tagged. Check:

```bash
glab api "projects/timmertech%2Fjetson-ffmpeg/pipelines?ref=main&per_page=10" \
  | jq '.[] | {id, status, source, sha}'
```

If no full-tier pipeline succeeded on HEAD, start one
(`glab ci run -b main --variables HW_TIER:full`) and wait for it (600s polls).

The tag pipeline enforces the same full tier itself (`release:test:ffmpeg-<ver>`
jobs in the `release-test` stage; `dist:*` and `release` need all of them), so
a red hardware run can never produce a release. Keep the pre-tag check anyway:
it surfaces failures before a tag exists.

If everything is green and no open MRs → skip to Phase 3.

---

## Phase 2: Wait for MRs & Pipelines (autonomous)

This phase is the walk-away mechanism. It monitors until all blockers clear.

### 2a. Monitor open MRs

For each MR intended for the release:

1. Check if auto-merge is enabled. If not, enable it:
   ```bash
   glab mr merge <NR> --auto-merge
   ```
   (Wait for pipeline status `running` before setting auto-merge — `--auto-merge`
   returns HTTP 405 when pipeline is `pending`.)

2. Arm a Monitor on each MR pipeline:
   ```bash
   glab api "projects/timmertech%2Fjetson-ffmpeg/merge_requests/<IID>" \
     | jq '{state, merged_at, head_pipeline: .head_pipeline | {id, status}}'
   ```

3. Poll every ~5 min (use `/loop` or `ScheduleWakeup` with 270s delay to stay
   in cache). On each tick:
   - If MR merged → check post-merge main pipeline (Phase 2b).
   - If MR pipeline failed → **STOP and report**. Do not proceed. The user
     must fix the MR before the release can continue.
   - If MR still open + pipeline running → continue waiting.

### 2b. Monitor main pipeline after merge

After an MR merges, the MR pipeline is irrelevant. Monitor the main pipeline:

```bash
glab api "projects/timmertech%2Fjetson-ffmpeg/pipelines?ref=main&per_page=1" \
  | jq '.[0] | {id, status, sha}'
```

- If `success` → the builds-only blocker is cleared (the full-tier pre-tag gate
  is still required, see Pipeline check).
- If `failed` → **STOP and report**. Merge introduced a regression.
- If `running`/`pending` → continue waiting.

### 2c. All clear

When ALL MRs are merged AND the latest main pipeline is `success`:
- Pull main to get merge commits: `git pull origin main`
- Proceed to Phase 3.

**CRITICAL**: Never proceed to Phase 3 while any intended MR is unmerged or
any pipeline is not green.

---

## Phase 3: Validate Release Scope

Verify every planned item landed on main.

```bash
git log --oneline $(git describe --tags --abbrev=0)..main
```

Present the commit list to the user (if still connected) or log it.

Check that no unintended commits snuck in. The scope is:
- All commits since the previous tag.
- Cross-reference with open/closed issues if relevant.

---

## Phase 4: Create Release Artifacts

### 4a. Version source

The version is the tag itself; there is no version file to bump (libnvmpi's
CMake version moved to jetson-nvmpi). The libnvmpi release bundled into the
archives is `NVMPI_VERSION` in `.gitlab-ci.yml`: confirm it is the intended
jetson-nvmpi release before tagging.

**Gate:** run the sync check from `/bump-nvmpi` step 6. If it prints anything
(a current-version example differs from `NVMPI_VERSION`), stop the release and
fix via `/bump-nvmpi` step 4 first.

### 4b. Regenerate CHANGELOG.md

```bash
git-cliff --config cliff.toml --tag vX.Y.Z -o CHANGELOG.md
```

If `git-cliff` is not available, fall back:
```bash
git log --pretty='- %s' $(git describe --tags --abbrev=0)..HEAD > /tmp/notes.md
```
Then manually prepend to CHANGELOG.md with proper header.

### 4c. Review changes

Show diff of CHANGELOG.md. If user is connected, get
confirmation. If autonomous (walk-away mode), proceed — the version was
specified upfront.

---

## Phase 5: Commit, Tag & Push

### 5a. Commit

```bash
git add CHANGELOG.md
git commit -m "$(cat <<'EOF'
docs: update CHANGELOG for vX.Y.Z release
EOF
)"
```

**Rules:**
- Conventional Commits format.
- Match existing pattern: `docs: update CHANGELOG for vX.Y.Z release`.
- No `[ci skip]` or `[skip ci]` in commit message — ever.
- No `Co-Authored-By` or AI attribution.
- Release commits are exempt from smoke-all gate.

### 5b. Push main with ci.skip

```bash
git push origin main -o ci.skip
```

The release commit is docs-only. Without `-o ci.skip`, GitLab runs a redundant
main pipeline alongside the tag pipeline, wasting Jetson runner time.

### 5c. Create annotated tag and push

```bash
git tag -a vX.Y.Z -m "vX.Y.Z"
git push origin vX.Y.Z
```

The tag push triggers the release pipeline (`dist` + `release` stages).

---

## Phase 6: Monitor Release Pipeline

The tag pipeline builds each FFmpeg version, bundles the released libnvmpi, uploads archives
to S3 and GitLab Package Registry, creates the GitLab Release.

### 6a. Get pipeline ID

```bash
glab api "projects/timmertech%2Fjetson-ffmpeg/pipelines?ref=vX.Y.Z&per_page=1" \
  | jq '.[0] | {id, status}'
```

### 6b. Monitor until complete

Arm a Monitor or poll with ScheduleWakeup (270s intervals):

- `success` → proceed to Phase 7.
- `failed` → **STOP. Abort the release.**
  1. Delete the tag: `git tag -d vX.Y.Z && git push origin :refs/tags/vX.Y.Z`
  2. Cancel the pipeline: `glab api --method POST "projects/timmertech%2Fjetson-ffmpeg/pipelines/<ID>/cancel"`
  3. Report failure to user. Do not continue.
- `running` → continue waiting.

---

## Phase 7: Verify Release

After the tag pipeline succeeds:

```javascript
commands: [
  { label: "gitlab-release", command: "glab api 'projects/timmertech%2Fjetson-ffmpeg/releases/vX.Y.Z' | jq '{name, tag_name, created_at, assets: (.assets.links | length)}'" },
  { label: "github-tag",     command: "gh api repos/gjrtimmer/jetson-ffmpeg/git/refs/tags/vX.Y.Z 2>&1 | head -5" },
  { label: "github-release", command: "gh release view vX.Y.Z -R gjrtimmer/jetson-ffmpeg 2>&1 | head -10" }
]
```

### Verification checklist

- [ ] GitLab release exists with correct tag and assets
- [ ] GitHub mirror has the tag (auto-synced)
- [ ] GitHub release `vX.Y.Z` exists with the archives + CHANGELOG.md (created by the `release:github` job; never by hand)
- [ ] CHANGELOG.md on main matches the release notes

Report final status to user.

---

## Abort Procedure

If anything fails after the tag is pushed:

1. Delete GitLab release: `glab api --method DELETE "projects/timmertech%2Fjetson-ffmpeg/releases/vX.Y.Z"`
2. Delete remote tag: `git push origin :refs/tags/vX.Y.Z`
3. Delete local tag: `git tag -d vX.Y.Z`
4. Cancel pipeline if running.
5. Report what went wrong.

GitLab auto-mirrors tag deletions to GitHub. A GitHub release already created by `release:github` is not removed by that: delete it too (`gh release delete vX.Y.Z -R gjrtimmer/jetson-ffmpeg`).

The release commit on main can stay (it's just a changelog update). Fix the
underlying issue, then re-run the skill.

---

## TodoWrite Checklist

Create on invocation:

1. Parse version argument, validate format
2. Pre-flight: auth, clean tree, no existing tag, version sanity
3. Check open MRs — ask user if they're release-blocking
4. Wait for MRs to merge (if any) — monitor pipelines
5. Wait for main pipeline green AND a full-tier (`HW_TIER=full`) pipeline green on the HEAD SHA
6. Validate release scope — list commits since last tag
7. Confirm `NVMPI_VERSION` (bundled libnvmpi release) and run the `/bump-nvmpi`
   step 6 sync check; any output stops the release
8. Regenerate CHANGELOG.md with git-cliff
9. Commit: `docs: update CHANGELOG for vX.Y.Z release`
10. Push main with `-o ci.skip`
11. Create annotated tag `vX.Y.Z`, push tag
12. Monitor release pipeline until green
13. Verify: GitLab release + GitHub mirror + assets
14. Report final status
