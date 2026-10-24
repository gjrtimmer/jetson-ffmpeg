---
name: bump-nvmpi
description: >-
  Adopt a new jetson-nvmpi release in jetson-ffmpeg: bump NVMPI_VERSION and
  keep every doc, example and wiki mention in sync. Invoke with
  "/bump-nvmpi [X.Y.Z]" (latest release if omitted).
argument-hint: "[X.Y.Z]"
user-invocable: true
---
# bump-nvmpi

Adopt jetson-nvmpi `<ver>`. Invoke: **`/bump-nvmpi 4.6.5`** (no argument = latest).
The pin bump is a **code change** (full MR pipeline), not docs-only. No tags,
no releases, no version bump of jetson-ffmpeg itself.

## 1. Resolve target and decide

```bash
gh release list -R gjrtimmer/jetson-nvmpi --limit 5
grep -n 'NVMPI_VERSION:' .gitlab-ci.yml
```

Read the jetson-nvmpi CHANGELOG between the current pin and the target. Apply
the `NVMPI_VERSION` policy from CLAUDE.md: bump only if the range changes
libnvmpi itself (`src/`, packaging). Mock/test/docs-only releases are not
followed. If the pin is bumped anyway, the docs must follow (step 4).

## 2. Coordinate

Per CLAUDE.md: `glab mr list` / `glab ci list` in jetson-ffmpeg and
jetson-nvmpi, message peer sessions before pushing or opening an MR.

## 3. Branch and pin

`git branch --show-current`, then branch `chore/<NR>-nvmpi-<ver>` from `main`
(NR = GitHub issue if one exists). Confirm the release has both
`jetson-nvmpi_<ver>_l4t<tag>_aarch64.tar.gz` assets. Edit `NVMPI_VERSION` in
`.gitlab-ci.yml` (`NVMPI_MOCK_REF` derives from it).

## 4. Sync mentions

Update every "current version" example to the new pin. Known spots: CLAUDE.md
`fetch-nvmpi.sh --version` example, `scripts/build.sh` usage,
`scripts/fetch-nvmpi.sh` usage. Leave feature floors ("X or newer" for a
feature, e.g. README AV1), `scripts/package.sh` (unrelated version) and
CHANGELOG history alone. Comment/usage text only, no logic changes.

## 5. Wiki

Clone `jetson-ffmpeg.wiki.git` per `.claude/agents/vcs-cli.md`; grep the pages
for nvmpi versions. If the release raises a minimum requirement (new API used
by the FFmpeg layer), update the floors consistently (Build-and-Install
"jetson-ffmpeg vX+ need jetson-nvmpi Y", FAQ). Push the wiki only with the MR
merge.

## 6. Sync check (must print nothing)

```bash
PIN=$(sed -nE 's/^ *NVMPI_VERSION: *"?([0-9.]+)"?.*/\1/p' .gitlab-ci.yml | head -1)
git grep -nE '(--version|e\.g\.) +[0-9]+\.[0-9]+\.[0-9]+' -- CLAUDE.md README.md \
  test/README.md .devcontainer/ .claude/ scripts/build.sh scripts/fetch-nvmpi.sh \
  | grep -i nvmpi | grep -vF "$PIN"
```

Any output is a stale example: fix it (step 4) and re-run. Add new files that
carry examples to the file list.

## 7. MR flow

Per CLAUDE.md: `git push -u origin <branch> -o ci.skip`, `glab mr create`, wait
until the MR pipeline is `running`, `glab mr merge <nr> --auto-merge`, arm a
pipeline monitor, then watch the post-merge `main` pipeline. Commit footer
`Closes #N` if an issue exists. Report back; no tags/releases.
