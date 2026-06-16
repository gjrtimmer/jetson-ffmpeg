# TODO

## ~~Encoder modular split~~ (Done)

Completed in [#29](https://github.com/gjrtimmer/jetson-ffmpeg/issues/29).
Mirrors decoder pattern: `nvmpi_enc_internal.h`, `nvmpi_enc_output.cpp`,
`nvmpi_enc_api.cpp`.

## Refactor: `NVMPI_frameBuf` naming consistency

Two naming issues in the frame-buffer component:

1. **Case**: `NVMPI_frameBuf` uses mixed case while all other types use
   `NVMPI_` prefix with lowercase (`nvmpictx`, `NVMPI_bufPool`).
   Rename to `nvmpi_framebuf` or similar.
2. **Abbreviation**: `frameBuf` is abbreviated; consider `frameBuffer` for
   clarity and consistency with the full-word style used elsewhere.

Affects: `src/NVMPI_frameBuf.hpp`, `src/NVMPI_frameBuf.cpp`, all include
sites in decoder/encoder source files.

## Encoder blocking wait (future)

`nvmpi_encoder_get_packet()` has a similar non-blocking pattern. The pool
changes (optional blocking dequeue) are generic and could be adopted by the
encoder. Out of scope for #10.

## Expand test coverage

Tracked in [#27](https://github.com/gjrtimmer/jetson-ffmpeg/issues/27):
additional format suites, JPEG decode, libnvmpi standalone API harness.
