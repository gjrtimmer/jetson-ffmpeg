# scripts/

Operator scripts for jetson-ffmpeg. Each resolves the repository root from its
own location, so it can be run from any working directory.

| Script | Purpose |
|--------|---------|
| [`build.sh`](build.sh) | Build / install the libnvmpi library (auto-detects real Jetson libs vs `stubs/`). |
| [`ffpatch.sh`](ffpatch.sh) | Patch a vanilla FFmpeg source tree with nvmpi codec support. |

Patch-development scripts live next to the data they operate on, under
[`../ffmpeg/dev/`](../ffmpeg/dev/) (`update_patch.sh`, `try_build.sh`,
`copy_files.sh`). The hardware smoke test lives in [`../test/`](../test/).

In the dev container these are exposed as the aliases `build`, `ffpatch`,
`update-patch`, `try-build`, and `hw-test`/`test`.

**Full reference (options, examples, aliases): [Scripts and Commands](https://github.com/gjrtimmer/jetson-ffmpeg/wiki/Scripts-and-Commands).**
