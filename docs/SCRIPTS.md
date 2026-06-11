# Scripts, Commands & Aliases

Reference for every script in the repository, the commands they expose, and the
dev-container aliases that wrap them.

All scripts resolve the repository root from their own location (via
`BASH_SOURCE`), so they can be run from **any working directory** — there is no
need to `cd` anywhere first.

---

## Quick reference

| Alias (dev container) | Script | Purpose |
|-----------------------|--------|---------|
| `build` | `scripts/build.sh` | Build / install the libnvmpi library |
| `ffpatch` | `scripts/ffpatch.sh` | Patch a vanilla FFmpeg source tree |
| `update-patch` | `ffmpeg/dev/update_patch.sh` | Regenerate the committed FFmpeg patches |
| `try-build` | `ffmpeg/dev/try_build.sh` | Build-validate every supported FFmpeg version |
| `hw-test` / `test` | `test/hw-test.sh` | Hardware encode/decode smoke test (one ffmpeg) |
| _(dev only)_ | `ffmpeg/dev/copy_files.sh` | Copy overlays into cloned FFmpeg trees |
| _(test)_ | `test/clone-ffmpeg.sh` | Clone the supported FFmpeg release branches |
| _(test)_ | `test/smoke-all.sh` | Full build + hw-test across **all** versions |

The aliases are defined in [`.devcontainer/bashrc`](../.devcontainer/bashrc),
which `postCreateCommand` concatenates into `~/.bashrc`, so they are present in
every freshly created dev container. They reference `JETSON_FFMPEG_ROOT`
(defaults to `/workspace`, the container's workspace mount).

---

## `scripts/build.sh` — build libnvmpi

Wraps the CMake build documented in [BUILD.md](BUILD.md). By default it builds
against the real Jetson Multimedia API libraries; if those are not present
(e.g. building off-Jetson or in CI) it automatically falls back to the stubs in
`stubs/` so the library still compiles (but is **not** runnable).

```bash
scripts/build.sh [options]
```

| Option | Description |
|--------|-------------|
| `--stubs` | Force linking against `stubs/` (`-DWITH_STUBS=ON`) |
| `--no-stubs` | Force linking against the real Jetson libraries |
| `--install` | Run the install step (uses `sudo` if not root) + `ldconfig` |
| `--clean` | Remove the build directory before configuring |
| `--build-dir DIR` | Build directory (default: `<repo>/build`) |
| `--prefix DIR` | `CMAKE_INSTALL_PREFIX` (default: CMake default, `/usr/local`) |
| `--build-type TYPE` | `CMAKE_BUILD_TYPE` (default: `Release`) |
| `-j N` / `-jN` | Parallel build jobs (default: `nproc`) |
| `-h`, `--help` | Show usage |

The `JETSON_MULTIMEDIA_API_DIR` and `JETSON_MULTIMEDIA_LIB_DIR` environment
variables override the paths used for stub auto-detection.

Examples:

```bash
build                       # native build (or stubs off-Jetson)
build --install             # build then install + ldconfig
build --stubs --clean       # fresh stubs build (CI / non-Jetson)
build --prefix ~/_nvmpi     # install into a custom prefix
```

---

## `scripts/ffpatch.sh` — patch a FFmpeg tree

The runtime patcher end users run against their own FFmpeg checkout. It
auto-detects the FFmpeg `libavcodec` version, inserts the nvmpi entries into
`configure`, `libavcodec/Makefile`, and `libavcodec/allcodecs.c` via `sed`
(idempotent), and copies the nvmpi codec sources from
`ffmpeg/dev/common/libavcodec/` into `libavcodec/`.

```bash
scripts/ffpatch.sh /path/to/ffmpeg
cd /path/to/ffmpeg && ./configure --enable-nvmpi && make
```

It works with any FFmpeg release from 4.2 onwards. See
[DEVELOPMENT.md](DEVELOPMENT.md#patch-system) for the patch system internals and
[the troubleshooting section](DEVELOPMENT.md#troubleshooting) when an anchor
fails.

---

## `ffmpeg/dev/update_patch.sh` — regenerate patches

Clones each supported FFmpeg release (shallow, into gitignored dirs under
`ffmpeg/dev/`), applies `scripts/ffpatch.sh`, and writes the resulting
`git diff` to `ffmpeg/patches/ffmpeg<ver>_nvmpi.patch`.

```bash
ffmpeg/dev/update_patch.sh        # alias: update-patch
```

The supported versions are listed once in the `VERSIONS` variable at the top of
the script (shared convention with `copy_files.sh` and `try_build.sh`). Existing
clones are reused rather than re-cloned.

> **Never hand-edit `ffmpeg/patches/*.patch`** — they are generated artifacts.
> Edit the sources under `ffmpeg/dev/` and regenerate.

---

## `ffmpeg/dev/try_build.sh` — build-validate all versions

Runs `update_patch.sh`, then `./configure --enable-nvmpi && make` in each cloned
FFmpeg tree. Exits non-zero on the first failure.

```bash
ffmpeg/dev/try_build.sh           # alias: try-build
```

---

## `ffmpeg/dev/copy_files.sh` — refresh a clone (dev helper)

Copies the per-version overlay (`ffmpeg/dev/<ver>/`) and the shared codec
sources (`ffmpeg/dev/common/`) into the matching cloned tree. Used during
iterative development to refresh a checkout without regenerating patches.

```bash
ffmpeg/dev/copy_files.sh
```

---

## `test/hw-test.sh` — hardware smoke test

Verifies that the nvmpi encoders/decoders are present and that a real hardware
transcode (h264_nvmpi decode → hevc_nvmpi / h264_nvmpi encode) succeeds.
Requires a real Jetson — there is no software fallback for nvmpi codecs.

```bash
JETSON_VARIANT=orin-nano test/hw-test.sh   # alias: hw-test (or test)
```

`JETSON_VARIANT` is informational (used in CI to label the runner) and defaults
to `unknown`.

---

## `test/clone-ffmpeg.sh` — fetch FFmpeg sources

Shallow-clones the supported FFmpeg release branches into a scratch directory
(idempotent — existing trees are skipped). Used by `smoke-all.sh`, but can be
run standalone.

```bash
test/clone-ffmpeg.sh [-d DIR] [-u URL] [VERSION ...]
```

Defaults: `DIR=$FFMPEG_SRC_DIR` (else `$HOME/ffmpeg-smoke`),
`URL=https://git.ffmpeg.org/ffmpeg.git`, and the full supported version set
when no versions are given.

---

## `test/smoke-all.sh` — full cross-version smoke test

The heaviest test in the repo. For **every** supported FFmpeg version it:
builds + installs libnvmpi → ensures a clone (`clone-ffmpeg.sh`) → resets it to
pristine → patches it (`scripts/ffpatch.sh`) → `./configure --enable-nvmpi
--enable-gpl --enable-libx264` → builds → runs the real `hw-test.sh`. Prints a
pass/fail matrix and exits non-zero on any failure.

```bash
test/smoke-all.sh [-d DIR] [-j N] [-v "4.2 6.0 8.0"] [--no-nvmpi]
```

Requires a real Jetson (the hw-test stages have no software fallback) and the
FFmpeg build dependencies incl. `libx264` (the dev container and CI install
these). Because it builds libnvmpi and a full FFmpeg per version, expect it to
run for many minutes.
