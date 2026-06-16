# TODO

## ~~Implement blocking `wait` in `nvmpi_decoder_get_frame()`~~ (Done)

Completed in [#10](https://github.com/gjrtimmer/jetson-ffmpeg/issues/10).
Condition-variable blocking dequeue in `NVMPI_bufPool`, atomic `eos`, shutdown
wiring, configurable `wait_timeout` AVOption (50–5000ms, default 500ms).

See [docs/THREAD_SAFETY.md](docs/THREAD_SAFETY.md) and
[docs/API_REFERENCE.md](docs/API_REFERENCE.md).

---

## Encoder modular split

Apply the same modular file pattern from the decoder refactor to the encoder:

- Extract encoder struct to `nvmpi_enc_internal.h`
- Extract capture callback to `nvmpi_enc_output.cpp`
- Rename `nvmpi_enc.cpp` to `nvmpi_enc_api.cpp`
- Update CMakeLists.txt, ARCHITECTURE.md, DEVELOPMENT.md

Tracked in a separate issue (TBD).

## Encoder blocking wait (future)

`nvmpi_encoder_get_packet()` has a similar non-blocking pattern. The pool
changes (optional blocking dequeue) are generic and could be adopted by the
encoder. Out of scope for #10.

## Expand test coverage

Tracked in [#27](https://github.com/gjrtimmer/jetson-ffmpeg/issues/27):
additional format suites, JPEG decode, libnvmpi standalone API harness.
