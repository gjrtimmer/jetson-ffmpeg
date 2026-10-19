# scripts/

Operator scripts for jetson-ffmpeg. Each resolves the repository root from its
own location, so it can be run from any working directory.

| Script | Purpose |
|--------|---------|
| [`build.sh`](build.sh) | Patch and build a full FFmpeg with nvmpi (`--ffmpeg VER`); libnvmpi comes from jetson-nvmpi (`--nvmpi-prefix`). |
| [`fetch-nvmpi.sh`](fetch-nvmpi.sh) | Download + verify a released jetson-nvmpi package (libnvmpi) into a prefix. |
| [`ffpatch.sh`](ffpatch.sh) | Patch a vanilla FFmpeg source tree with nvmpi codec support. |

Patch-development scripts live next to the data they operate on, under
[`../ffmpeg/dev/`](../ffmpeg/dev/) (`update_patch.sh`, `try_build.sh`,
`copy_files.sh`). The hardware smoke test lives in [`../test/`](../test/).

In the dev container these are exposed as the aliases `build`, `ffpatch`,
`update-patch`, `try-build`, and `hw-test`/`test`.

**Full reference (options, examples, aliases): [Scripts and Commands](https://github.com/gjrtimmer/jetson-ffmpeg/wiki/Scripts-and-Commands).**
