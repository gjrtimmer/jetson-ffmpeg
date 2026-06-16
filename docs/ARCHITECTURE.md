# Architecture

## Modular File Structure

libnvmpi source files follow a naming convention that keeps each file focused
on one concern. The convention applies to both the decoder and encoder.

### Naming pattern

`nvmpi_{codec}_{concern}.cpp` where `{codec}` is `dec` or `enc`:

| Concern | Suffix | Decoder | Encoder |
|---------|--------|---------|---------|
| Public API | `_api` | `nvmpi_dec_api.cpp` | `nvmpi_enc_api.cpp` |
| Capture/output loop | `_capture` / `_output` | `nvmpi_dec_capture.cpp` | `nvmpi_enc_output.cpp` |
| V4L2 plane management | `_planes` | `nvmpi_dec_planes.cpp` | — |
| Internal header | `_internal.h` | `nvmpi_dec_internal.h` | `nvmpi_enc_internal.h` |

Shared infrastructure:

| File | Contents |
|------|----------|
| `NVMPI_bufPool.hpp` | Generic thread-safe producer/consumer buffer pool (header-only template) |
| `NVMPI_frameBuf.hpp` / `.cpp` | DMA buffer allocation and destruction |

### Rules

1. **Internal headers** (`*_internal.h`) hold the context struct (`nvmpictx`)
   and internal forward declarations. Located in `src/`, NOT installed.
2. **Public API** (`include/nvmpi.h`) is the only installed header — no ABI
   break from internal refactors.
3. Each `.cpp` file must be independently compilable — no circular includes.
4. `CMakeLists.txt` lists source files explicitly (no globs).
5. **When to split further:** a file exceeds ~500 lines or gains a new logical
   concern (e.g., a new thread, a new V4L2 device interaction).

### Include structure

```text
include/nvmpi.h          <- public, installed
include/NVMPI_bufPool.hpp <- public (used by both dec and enc)
include/NVMPI_frameBuf.hpp
src/nvmpi_dec_internal.h  <- private, includes all of the above + NVIDIA headers
src/nvmpi_dec_api.cpp     <- includes nvmpi_dec_internal.h
src/nvmpi_dec_capture.cpp <- includes nvmpi_dec_internal.h
src/nvmpi_dec_planes.cpp  <- includes nvmpi_dec_internal.h
```

### Encoder files

The encoder follows the same modular pattern:

| File | Concern |
|------|---------|
| `src/nvmpi_enc_api.cpp` | Public API (`create`, `put_frame`, `get_packet`, `close`, packet pool ops) |
| `src/nvmpi_enc_output.cpp` | DQ-thread callback, output-plane DMA buffer setup |
| `src/nvmpi_enc_internal.h` | Context struct, macros (`TEST_ERROR`, `MAX_BUFFERS`, `OUTPLANE_MEMTYPE_*`), forward declarations |
