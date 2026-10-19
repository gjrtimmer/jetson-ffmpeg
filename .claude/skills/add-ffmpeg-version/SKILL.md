---
name: add-ffmpeg-version
description: >-
  Ordered checklist for adding support for a new FFmpeg major/minor (e.g. 9.0)
  to jetson-ffmpeg: scripts, ffpatch anchors, common codec guards, regenerated
  patches, CI jobs, docs. Invoke with "/add-ffmpeg-version <version>".
argument-hint: "<version, e.g. 9.0>"
user-invocable: true
---
# add-ffmpeg-version

Add FFmpeg `<ver>` (example: `9.0`) end to end. Invoke: **`/add-ffmpeg-version 9.0`**.
Keep this in sync with the wiki Development-Guide section
"Adding Support for a New FFmpeg Version" and the comment "To add a new FFmpeg
version" in `.gitlab-ci.yml`.

## 0. Flow (per CLAUDE.md)

- Create a GitHub issue on `gjrtimmer/jetson-ffmpeg` (labels: type, `area:ffmpeg`,
  priority). Post the plan comment before starting. Use the `fix-issue` skill
  gates for branch/commit/MR mechanics.
- Branch from `main`: `feat/{NR}-ffmpeg-<ver>`. Check `git branch --show-current`
  before every edit.
- Commits split by concern: (1) implementation (scripts, guards, regenerated
  patches; `Fixes #N` footer), (2) tests, (3) CI/docs. Conventional Commits, no
  attribution lines.
- Never edit `CHANGELOG.md` and never tag or release: the changelog is generated
  at release time only.

## 1. Version lists (one edit per file)

Add `<ver>` to the version list in each:

- `scripts/clone-ffmpeg.sh` (`DEFAULT_VERSIONS`)
- `ffmpeg/dev/update_patch.sh` (`VERSIONS`)
- `ffmpeg/dev/try_build.sh` (`VERSIONS`)
- `test/smoke-all.sh` (`VERSIONS`)

`ffmpeg/dev/copy_files.sh` only refreshes the `6.0` dev overlay: leave it.
Then `grep -rn "8\.1" --exclude-dir=.git --exclude-dir=.work --exclude=CHANGELOG.md .`
(use the current newest version) to catch any list not named above.

## 2. Clone and patch

```bash
scripts/clone-ffmpeg.sh <ver>                    # release/<ver> into $FFMPEG_SRC_DIR
scripts/ffpatch.sh "$FFMPEG_SRC_DIR/ffmpeg<ver>" # reports which anchor moved
```

`scripts/ffpatch.sh` is the single patch source. It reads `libavcodec/version.h`
/ `version_major.h`, refuses libavcodec < 60, and `sed`-inserts against anchor
strings in `configure`, `libavcodec/Makefile`, `libavcodec/allcodecs.c`,
`libavutil/` (hwcontext) and `libavfilter/` (`Makefile`, `allfilters.c`; FFFilter
from libavfilter 11). Fix any moved anchor in `ffpatch.sh` and re-check that every
older version still patches. A per-version overlay under `ffmpeg/dev/<ver>/`
(only `6.0` exists) is a last resort, for a difference `sed` cannot express.

## 3. Common codec sources

Edit shared code in `ffmpeg/dev/common/libavcodec/` (`nvmpi_enc.c`,
`nvmpi_dec.c`, `nvmpi_enc_runtime.c`, `nvmpi_enc_ff_internal.h`, ...) and
`ffmpeg/dev/common/libavfilter/` only if the new FFmpeg breaks the build.

- Prefer feature detection (`#ifndef SYMBOL`) over `MAJOR >= X && MINOR >= Y`
  arithmetic: MINOR resets to 0 on a MAJOR bump.
- Comment every `#if LIBAVCODEC_VERSION_MAJOR` / `#ifdef WITH_NVUTILS` block
  with the FFmpeg/JetPack versions it handles. Follow the secure-codec rules in
  CLAUDE.md.
- If the change forks wrapper code (a new compile path), update the "Wrapper code
  paths by FFmpeg version" table in the wiki Development-Guide.

## 4. Regenerate patches and build-validate

```bash
./ffmpeg/dev/update_patch.sh   # writes ffmpeg/patches/ffmpeg<ver>_nvmpi.patch
./ffmpeg/dev/try_build.sh      # configure --enable-nvmpi && make, every version
```

Never hand-edit `ffmpeg/patches/*.patch`. Commit source edits and regenerated
patches together. Build one old (6.0) and one new version locally to prove the
guards: `./scripts/build.sh --ffmpeg <ver>`.

## 5. Local hardware loop

```bash
./scripts/build.sh --install
./scripts/build.sh --ffmpeg <ver>
export PATH="/home/vscode/ffmpeg-build/ffmpeg<ver>:$PATH"
ls test/hw-*.sh                                  # verify suite names first
HW_SUITES="<suite>" JETSON_VARIANT=orin-nano ./test/hw-all.sh
./test/smoke-all.sh -v "<ver>" 2>&1 | tee /tmp/smoke-<ver>.log
```

Check `glab ci list` first: never run `hw-all.sh`/`smoke-all.sh` while a pipeline
runs hw tests on the same Jetson (the Orin is shared; see CLAUDE.md). Re-run only
a failed version or suite.

## 6. CI (`.gitlab-ci.yml`)

Use these names exactly as they exist today (verify with `grep -n "ffmpeg-8.1"`):

- `FFVER` matrices: `ffmpeg:jp6`, `dist:jp6`, `dist:jp5` get `"<ver>"`.
  `ffmpeg:jp5` is a compile check on the oldest and newest version only
  (`["6.0", "8.1"]`): replace the newest entry if `<ver>` is newer.
- New jobs, each with a `needs:` on its own `ffmpeg:jp6` matrix entry (copy the
  8.0 block): `test:stub:ffmpeg-<ver>` (extends `.stub-test`; also needs
  `nvmpi-mock:jp6`), `test:hw-fast:ffmpeg-<ver>` (extends `.hw-fast`) and
  `test:hw-full:ffmpeg-<ver>` (extends `.hw-full-twin`).
- Full-tier carrier: 7.1 and 8.1 run full in every MR (one per wrapper path).
  If `<ver>` shares a path with the current carrier and is newer, move the
  carrier role to it: `test:hw-full:ffmpeg-<ver>` extends `.hw-full`, and the old
  carrier becomes a `test:hw-fast:ffmpeg-<old>` job plus a
  `test:hw-full:ffmpeg-<old>` twin (`.hw-full-twin`). A new wrapper path adds a
  third carrier instead. Update the stage comments and the `HW_TIER` variable
  description that name 7.1/8.1.
- Release gate: add `release:test:ffmpeg-<ver>` (extends
  `["test:hw-full:ffmpeg-<ver>", ".release-gate-job"]`), and list it in
  `.release-gate` and in the `release` job's `needs`. Every version needs a
  `test:hw-full:ffmpeg-<ver>` job because the gate extends it.
- Unchanged: `HW_SUITES_EXCLUDE` (only `soak-decode`), `MOCK_SUITES` in
  `test/hw-all.sh`, `.code-paths`.
- Update the "To add a new FFmpeg version" comment if the steps changed.
- `glab ci lint`, then open an MR with `-o ci.skip` on the first push and
  auto-merge once its pipeline is running (CLAUDE.md MR rules). The MR pipeline
  (builds + stub + hw tiered) is the validation; a weekly or web `HW_TIER=full`
  run proves every version in full before the next tag.

## 7. Docs

- `README.md` (supported versions text, example clone line if relevant).
- Wiki pages (clone `jetson-ffmpeg.wiki.git`): Compatibility ("FFmpeg Support"),
  Development-Guide (wrapper-paths table, this checklist), GitLab-CI (job names,
  tier tables), FAQ-Versions-and-Compatibility, Build-and-Install if it lists
  versions, Release-Process (gate/dist version lists).
- `CLAUDE.md`: the `Supported FFmpeg versions` line.
- Do not touch `CHANGELOG.md`.

## 8. Dropping a version (retire policy)

Mark it **retired**, never erase it: keep a "Retired FFmpeg versions" entry in the
wiki Compatibility page with a pointer to the last release that contained it
(like 4.2/4.4 -> v2.10.0), remove it from the lists in steps 1 and 6, delete its
patch file, and make `scripts/ffpatch.sh` refuse it with a message naming that
release if it no longer works.

## 9. Finish

Issue closes only through the `Fixes #N` footer on a commit that lands on `main`.
Before pushing the branch run `/retro`. Post the closing-detail comment (what,
where, how validated) once the pipeline is green and the user confirms. Report
status per repo: committed, pushed, leftovers.
