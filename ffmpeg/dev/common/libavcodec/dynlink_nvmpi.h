/*
 * dynlink_nvmpi.h -- Runtime lazy-loading of libnvmpi via dlopen/dlsym.
 *
 * This header is SELF-CONTAINED: it duplicates all type definitions from
 * the libnvmpi public API (nvmpi.h) so the FFmpeg codec wrappers can
 * compile without any libnvmpi development files installed.  The shared
 * library is loaded at runtime on first codec use; if absent, the codec
 * gracefully reports "unavailable" and FFmpeg continues.
 *
 * Usage: #include "dynlink_nvmpi.h" instead of <nvmpi.h> in the FFmpeg
 * wrapper files (nvmpi_dec.c, nvmpi_enc.c, nvmpi_enc_jpeg.c).
 *
 * Load sequence:
 *   1. Codec .init calls nvmpi_dynlink_load()
 *      -- returns 0 on success, -1 on failure (sets dlerror()).
 *   2. If load fails, codec returns AVERROR_EXTERNAL with a descriptive
 *      av_log message; FFmpeg continues without nvmpi codecs.
 *   3. All nvmpi_* function calls transparently dispatch through the
 *      loaded pointers via macro redirects (existing code unchanged).
 *   4. The library stays loaded for the process lifetime (no dlclose).
 *      This is intentional: multiple codec instances may share the
 *      handle concurrently, and the memory cost is negligible.
 *
 * Thread safety: dlopen/dlsym are thread-safe on Linux.  Concurrent
 * calls to nvmpi_dynlink_load() from different codec instances are
 * benign -- each resolves the same symbols to the same addresses, and
 * dlopen is reference-counted internally.
 *
 * Ported and improved from the YuriiHoliuk/jetson-ffmpeg fork's
 * dynlink_nvmpi.h approach.  Key differences:
 *   - Covers ALL 22 public nvmpi functions (inc. JPEG enc/dec, flush,
 *     force_idr, set_bitrate).
 *   - No dlclose (safe with concurrent codec instances).
 *   - Self-contained types kept in sync with gjrtimmer/jetson-ffmpeg's
 *     nvmpi.h, not YuriiHoliuk's extended API.
 */
#ifndef DYNLINK_NVMPI_H
#define DYNLINK_NVMPI_H

#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <dlfcn.h>

/* ------------------------------------------------------------------ */
/* Type definitions (duplicated from nvmpi.h -- keep in sync)          */
/* ------------------------------------------------------------------ */

/* Maximum encoded-frame buffer size in bytes.  Shared with nvmpi.h;
 * used by the encoder wrapper for packet pool allocation. */
#define NVMPI_ENC_CHUNK_SIZE 10*1024*1024

/* Opaque context handle.  Decoder and encoder each define their own
 * internal struct; a handle must only be passed back to the API family
 * (decoder_* or encoder_*) that created it. */
typedef struct nvmpictx nvmpictx;

/* Raw pixel layouts: decoder output / encoder input. */
typedef enum {
    NV_PIX_NV12,
    NV_PIX_YUV420,
    NV_PIX_P010
} nvPixFormat;

/* Compressed bitstream codec selector. */
typedef enum {
    NV_VIDEO_CodingUnused,
    NV_VIDEO_CodingH264,
    NV_VIDEO_CodingMPEG4,
    NV_VIDEO_CodingMPEG2,
    NV_VIDEO_CodingVP8,
    NV_VIDEO_CodingVP9,
    NV_VIDEO_CodingHEVC,
    NV_VIDEO_CodingMJPEG,
} nvCodingType;

/* Simple width/height pair (decoder resize target). */
typedef struct _NVSIZE {
    unsigned int width;
    unsigned int height;
} nvSize;

/* Encoder creation parameters. */
typedef struct _NVENCPARAM {
    unsigned int width;
    unsigned int height;
    unsigned int profile;
    unsigned int level;
    unsigned int bitrate;
    unsigned int peak_bitrate;
    char enableLossless;
    char mode_vbr;
    char insert_spspps_idr;
    char insert_vui;
    nvPixFormat pixFormat;
    unsigned int iframe_interval;
    unsigned int idr_interval;
    unsigned int fps_n;
    unsigned int fps_d;
    int capture_num;
    unsigned int max_b_frames;
    unsigned int refs;
    unsigned int qmax;
    unsigned int qmin;
    unsigned int hw_preset_type;
    unsigned int vbv_buffer_size;
    nvCodingType codingType;
    int max_perf;
    unsigned int poc_type;
    unsigned int wait_timeout;
    char enable_cabac;
    char insert_aud;
    int use_dmabuf;
} nvEncParam;

/* Decoder creation parameters. */
typedef struct _NVDECPARAM {
    int frame_pool_size;
    nvCodingType codingType;
    nvPixFormat pixFormat;
    nvSize resized;
    unsigned int chunk_size;
    int max_perf;
    int disable_dpb;
    int wait_timeout;
    unsigned int width;
    unsigned int height;
} nvDecParam;

/* Compressed packet exchanged across the API boundary. */
typedef struct _NVPACKET {
    unsigned long flags;
    unsigned long payload_size;
    unsigned char *payload;
    unsigned long pts;
    void *privData;
} nvPacket;

/* Raw planar frame exchanged across the API boundary. */
typedef struct _NVFRAME {
    unsigned long flags;
    unsigned long payload_size[3];
    unsigned char *payload[3];
    unsigned int linesize[3];
    nvPixFormat type;
    unsigned int width;
    unsigned int height;
    time_t timestamp;
} nvFrame;

/* Release callback for borrowed DMA-BUF fds from get_frame_fd. */
typedef void (*nvmpi_frame_release_cb)(void *opaque);

/* ------------------------------------------------------------------ */
/* Function pointer typedefs                                           */
/* ------------------------------------------------------------------ */

/* Decoder */
typedef nvmpictx* (*fn_nvmpi_create_decoder)(nvDecParam *param);
typedef int       (*fn_nvmpi_decoder_put_packet)(nvmpictx *ctx, nvPacket *packet);
typedef int       (*fn_nvmpi_decoder_get_frame)(nvmpictx *ctx, nvFrame *frame, bool wait);
typedef int       (*fn_nvmpi_decoder_flush)(nvmpictx *ctx);
typedef int       (*fn_nvmpi_decoder_close)(nvmpictx *ctx);

/* JPEG decoder */
typedef nvmpictx* (*fn_nvmpi_create_jpeg_decoder)(void);
typedef int       (*fn_nvmpi_jpeg_decoder_put_packet)(nvmpictx *ctx, nvPacket *packet);
typedef int       (*fn_nvmpi_jpeg_decoder_get_frame)(nvmpictx *ctx, nvFrame *frame, bool wait);
typedef int       (*fn_nvmpi_jpeg_decoder_close)(nvmpictx *ctx);

/* Encoder */
typedef nvmpictx* (*fn_nvmpi_create_encoder)(nvEncParam *param);
typedef int       (*fn_nvmpi_encoder_put_frame)(nvmpictx *ctx, nvFrame *frame);
typedef int       (*fn_nvmpi_encoder_get_packet)(nvmpictx *ctx, nvPacket **packet, bool wait);
typedef int       (*fn_nvmpi_encoder_dqEmptyPacket)(nvmpictx *ctx, nvPacket **packet);
typedef void      (*fn_nvmpi_encoder_qEmptyPacket)(nvmpictx *ctx, nvPacket *packet);
typedef int       (*fn_nvmpi_encoder_force_idr)(nvmpictx *ctx);
typedef int       (*fn_nvmpi_encoder_set_bitrate)(nvmpictx *ctx, unsigned int bitrate);
typedef int       (*fn_nvmpi_encoder_flush)(nvmpictx *ctx);
typedef int       (*fn_nvmpi_encoder_close)(nvmpictx *ctx);

/* JPEG encoder */
typedef nvmpictx* (*fn_nvmpi_create_jpeg_encoder)(int quality);
typedef int       (*fn_nvmpi_jpeg_encoder_put_frame)(nvmpictx *ctx, nvFrame *frame);
typedef int       (*fn_nvmpi_jpeg_encoder_get_packet)(nvmpictx *ctx, nvPacket *packet);
typedef int       (*fn_nvmpi_jpeg_encoder_close)(nvmpictx *ctx);

/* DMA-BUF fd passthrough (zero-copy) */
typedef int       (*fn_nvmpi_decoder_get_frame_fd)(nvmpictx *ctx,
    int *dmabuf_fd, int *width, int *height, int *pitch,
    int64_t *timestamp, nvmpi_frame_release_cb *release,
    void **opaque, bool wait);
typedef int       (*fn_nvmpi_encoder_put_frame_fd)(nvmpictx *ctx,
    int dmabuf_fd, int width, int height, int pitch,
    int64_t timestamp);
typedef int       (*fn_nvmpi_surface_alloc)(unsigned int width,
    unsigned int height, int *dmabuf_fd);
typedef int       (*fn_nvmpi_surface_destroy)(int dmabuf_fd);

/* ------------------------------------------------------------------ */
/* Static function pointers (one set per translation unit)             */
/* ------------------------------------------------------------------ */

/* Decoder */
static fn_nvmpi_create_decoder           nvmpi_create_decoder_dl;
static fn_nvmpi_decoder_put_packet       nvmpi_decoder_put_packet_dl;
static fn_nvmpi_decoder_get_frame        nvmpi_decoder_get_frame_dl;
static fn_nvmpi_decoder_flush            nvmpi_decoder_flush_dl;
static fn_nvmpi_decoder_close            nvmpi_decoder_close_dl;

/* JPEG decoder */
static fn_nvmpi_create_jpeg_decoder      nvmpi_create_jpeg_decoder_dl;
static fn_nvmpi_jpeg_decoder_put_packet  nvmpi_jpeg_decoder_put_packet_dl;
static fn_nvmpi_jpeg_decoder_get_frame   nvmpi_jpeg_decoder_get_frame_dl;
static fn_nvmpi_jpeg_decoder_close       nvmpi_jpeg_decoder_close_dl;

/* Encoder */
static fn_nvmpi_create_encoder           nvmpi_create_encoder_dl;
static fn_nvmpi_encoder_put_frame        nvmpi_encoder_put_frame_dl;
static fn_nvmpi_encoder_get_packet       nvmpi_encoder_get_packet_dl;
static fn_nvmpi_encoder_dqEmptyPacket    nvmpi_encoder_dqEmptyPacket_dl;
static fn_nvmpi_encoder_qEmptyPacket     nvmpi_encoder_qEmptyPacket_dl;
static fn_nvmpi_encoder_force_idr        nvmpi_encoder_force_idr_dl;
static fn_nvmpi_encoder_set_bitrate      nvmpi_encoder_set_bitrate_dl;
static fn_nvmpi_encoder_flush            nvmpi_encoder_flush_dl;
static fn_nvmpi_encoder_close            nvmpi_encoder_close_dl;

/* JPEG encoder */
static fn_nvmpi_create_jpeg_encoder      nvmpi_create_jpeg_encoder_dl;
static fn_nvmpi_jpeg_encoder_put_frame   nvmpi_jpeg_encoder_put_frame_dl;
static fn_nvmpi_jpeg_encoder_get_packet  nvmpi_jpeg_encoder_get_packet_dl;
static fn_nvmpi_jpeg_encoder_close       nvmpi_jpeg_encoder_close_dl;

/* DMA-BUF fd passthrough (zero-copy) */
static fn_nvmpi_decoder_get_frame_fd    nvmpi_decoder_get_frame_fd_dl;
static fn_nvmpi_encoder_put_frame_fd    nvmpi_encoder_put_frame_fd_dl;
static fn_nvmpi_surface_alloc           nvmpi_surface_alloc_dl;
static fn_nvmpi_surface_destroy         nvmpi_surface_destroy_dl;

/* Library handle (per-TU; dlopen is reference-counted internally). */
static void *nvmpi_lib_handle;

/* ------------------------------------------------------------------ */
/* Loader                                                              */
/* ------------------------------------------------------------------ */

/* Helper: resolve one symbol or jump to the fail label.
 * Uses dlsym + cast; the pragma suppresses pedantic warnings about
 * casting void* to function pointers (ISO C forbids it, POSIX requires
 * it, and every real compiler on Linux supports it). */
#define NVMPI_LOAD_SYM(ptr, name) do {                              \
    *(void **)&(ptr) = dlsym(nvmpi_lib_handle, #name);              \
    if (!(ptr)) goto fail;                                           \
} while (0)

/*
 * Load libnvmpi.so and resolve all function pointers.
 *
 * Returns 0 on success, -1 on failure.  On failure, dlerror() describes
 * the problem (missing library or missing symbol).  Subsequent calls
 * after a successful load return 0 immediately (idempotent).
 *
 * Thread safety: benign races -- concurrent callers may both dlopen and
 * resolve symbols, but the results are identical (same .so, same addrs).
 */
static int nvmpi_dynlink_load(void)
{
    if (nvmpi_lib_handle)
        return 0;

    /* RTLD_LAZY: defer symbol resolution until first call -- faster
     * startup when codecs are registered but never used. */
    nvmpi_lib_handle = dlopen("libnvmpi.so", RTLD_LAZY);
    if (!nvmpi_lib_handle)
        return -1;

    /* Decoder symbols */
    NVMPI_LOAD_SYM(nvmpi_create_decoder_dl,          nvmpi_create_decoder);
    NVMPI_LOAD_SYM(nvmpi_decoder_put_packet_dl,      nvmpi_decoder_put_packet);
    NVMPI_LOAD_SYM(nvmpi_decoder_get_frame_dl,       nvmpi_decoder_get_frame);
    NVMPI_LOAD_SYM(nvmpi_decoder_flush_dl,           nvmpi_decoder_flush);
    NVMPI_LOAD_SYM(nvmpi_decoder_close_dl,           nvmpi_decoder_close);

    /* JPEG decoder symbols */
    NVMPI_LOAD_SYM(nvmpi_create_jpeg_decoder_dl,     nvmpi_create_jpeg_decoder);
    NVMPI_LOAD_SYM(nvmpi_jpeg_decoder_put_packet_dl, nvmpi_jpeg_decoder_put_packet);
    NVMPI_LOAD_SYM(nvmpi_jpeg_decoder_get_frame_dl,  nvmpi_jpeg_decoder_get_frame);
    NVMPI_LOAD_SYM(nvmpi_jpeg_decoder_close_dl,      nvmpi_jpeg_decoder_close);

    /* Encoder symbols */
    NVMPI_LOAD_SYM(nvmpi_create_encoder_dl,          nvmpi_create_encoder);
    NVMPI_LOAD_SYM(nvmpi_encoder_put_frame_dl,       nvmpi_encoder_put_frame);
    NVMPI_LOAD_SYM(nvmpi_encoder_get_packet_dl,      nvmpi_encoder_get_packet);
    NVMPI_LOAD_SYM(nvmpi_encoder_dqEmptyPacket_dl,   nvmpi_encoder_dqEmptyPacket);
    NVMPI_LOAD_SYM(nvmpi_encoder_qEmptyPacket_dl,    nvmpi_encoder_qEmptyPacket);
    NVMPI_LOAD_SYM(nvmpi_encoder_force_idr_dl,       nvmpi_encoder_force_idr);
    NVMPI_LOAD_SYM(nvmpi_encoder_set_bitrate_dl,     nvmpi_encoder_set_bitrate);
    NVMPI_LOAD_SYM(nvmpi_encoder_flush_dl,           nvmpi_encoder_flush);
    NVMPI_LOAD_SYM(nvmpi_encoder_close_dl,           nvmpi_encoder_close);

    /* JPEG encoder symbols */
    NVMPI_LOAD_SYM(nvmpi_create_jpeg_encoder_dl,     nvmpi_create_jpeg_encoder);
    NVMPI_LOAD_SYM(nvmpi_jpeg_encoder_put_frame_dl,  nvmpi_jpeg_encoder_put_frame);
    NVMPI_LOAD_SYM(nvmpi_jpeg_encoder_get_packet_dl, nvmpi_jpeg_encoder_get_packet);
    NVMPI_LOAD_SYM(nvmpi_jpeg_encoder_close_dl,      nvmpi_jpeg_encoder_close);

    /* DMA-BUF fd passthrough symbols (zero-copy, issue #60) */
    NVMPI_LOAD_SYM(nvmpi_decoder_get_frame_fd_dl,   nvmpi_decoder_get_frame_fd);
    NVMPI_LOAD_SYM(nvmpi_encoder_put_frame_fd_dl,   nvmpi_encoder_put_frame_fd);
    NVMPI_LOAD_SYM(nvmpi_surface_alloc_dl,           nvmpi_surface_alloc);
    NVMPI_LOAD_SYM(nvmpi_surface_destroy_dl,         nvmpi_surface_destroy);

    return 0;

fail:
    /* A symbol was missing -- close the partially loaded library.
     * This should never happen with a matching libnvmpi version, but
     * protects against ABI mismatches between the header types above
     * and the installed .so. */
    dlclose(nvmpi_lib_handle);
    nvmpi_lib_handle = NULL;
    return -1;
}

/* ------------------------------------------------------------------ */
/* Macro redirects -- transparent dispatch to function pointers        */
/*                                                                     */
/* Existing code calls nvmpi_create_decoder(&param); these macros      */
/* expand that to nvmpi_create_decoder_dl(&param) without any source   */
/* changes in the calling files.                                       */
/* ------------------------------------------------------------------ */

/* Decoder */
#define nvmpi_create_decoder          nvmpi_create_decoder_dl
#define nvmpi_decoder_put_packet      nvmpi_decoder_put_packet_dl
#define nvmpi_decoder_get_frame       nvmpi_decoder_get_frame_dl
#define nvmpi_decoder_flush           nvmpi_decoder_flush_dl
#define nvmpi_decoder_close           nvmpi_decoder_close_dl

/* JPEG decoder */
#define nvmpi_create_jpeg_decoder     nvmpi_create_jpeg_decoder_dl
#define nvmpi_jpeg_decoder_put_packet nvmpi_jpeg_decoder_put_packet_dl
#define nvmpi_jpeg_decoder_get_frame  nvmpi_jpeg_decoder_get_frame_dl
#define nvmpi_jpeg_decoder_close      nvmpi_jpeg_decoder_close_dl

/* Encoder */
#define nvmpi_create_encoder          nvmpi_create_encoder_dl
#define nvmpi_encoder_put_frame       nvmpi_encoder_put_frame_dl
#define nvmpi_encoder_get_packet      nvmpi_encoder_get_packet_dl
#define nvmpi_encoder_dqEmptyPacket   nvmpi_encoder_dqEmptyPacket_dl
#define nvmpi_encoder_qEmptyPacket    nvmpi_encoder_qEmptyPacket_dl
#define nvmpi_encoder_force_idr       nvmpi_encoder_force_idr_dl
#define nvmpi_encoder_set_bitrate     nvmpi_encoder_set_bitrate_dl
#define nvmpi_encoder_flush           nvmpi_encoder_flush_dl
#define nvmpi_encoder_close           nvmpi_encoder_close_dl

/* JPEG encoder */
#define nvmpi_create_jpeg_encoder     nvmpi_create_jpeg_encoder_dl
#define nvmpi_jpeg_encoder_put_frame  nvmpi_jpeg_encoder_put_frame_dl
#define nvmpi_jpeg_encoder_get_packet nvmpi_jpeg_encoder_get_packet_dl
#define nvmpi_jpeg_encoder_close      nvmpi_jpeg_encoder_close_dl

/* DMA-BUF fd passthrough (zero-copy) */
#define nvmpi_decoder_get_frame_fd    nvmpi_decoder_get_frame_fd_dl
#define nvmpi_encoder_put_frame_fd    nvmpi_encoder_put_frame_fd_dl
#define nvmpi_surface_alloc           nvmpi_surface_alloc_dl
#define nvmpi_surface_destroy         nvmpi_surface_destroy_dl

#endif /* DYNLINK_NVMPI_H */
