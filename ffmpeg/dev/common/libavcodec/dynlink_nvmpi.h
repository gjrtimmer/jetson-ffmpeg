/*
 * dynlink_nvmpi.h -- Runtime lazy-loading of libnvmpi via dlopen/dlsym.
 *
 * This header is SELF-CONTAINED: it duplicates all type definitions from
 * the libnvmpi public API (nvmpi.h) so the FFmpeg codec wrappers can
 * compile without any libnvmpi development files installed.  The shared
 * library is loaded at runtime on first codec use; if absent, the codec
 * gracefully reports "unavailable" and FFmpeg continues.
 *
 * Usage: #include "dynlink_nvmpi.h" instead of <nvmpi.h> in the wrappers.
 *
 * Load sequence:
 *   1. Codec .init calls nvmpi_dynlink_load()
 *      -- returns 0 on success, -1 on failure (sets dlerror()).
 *   2. If load fails, codec returns AVERROR_EXTERNAL with a descriptive
 *      av_log message; FFmpeg continues without nvmpi codecs.
 *   3. All nvmpi_* function calls transparently dispatch through the
 *      loaded pointers via macro redirects (existing code unchanged).
 *   4. The library stays loaded for the process lifetime (no dlclose).
 *
 * Thread safety: concurrent nvmpi_dynlink_load() calls are benign (same
 * .so, same addresses; dlopen is reference-counted).
 *
 * Ported from the YuriiHoliuk/jetson-ffmpeg dynlink approach; no dlclose,
 * types kept in sync with nvmpi.h.
 */
#ifndef DYNLINK_NVMPI_H
#define DYNLINK_NVMPI_H

#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <dlfcn.h>

/* ------------------------------------------------------------------ */
/* Type definitions (duplicated from nvmpi.h -- keep in sync)          */
/* ------------------------------------------------------------------ */

/* Maximum encoded-frame buffer size in bytes (encoder packet pool). */
#define NVMPI_ENC_CHUNK_SIZE 10*1024*1024

/* Encoder would block: drain output packets and retry (Linux EAGAIN). */
#define NVMPI_ERR_EAGAIN (-11)

/* The decoded frame does not fit the caller's buffers (libnvmpi >= 4.6.0:
 * nvmpi_decoder_get_frame2, nvmpi_decoder_get_frame,
 * nvmpi_jpeg_decoder_get_frame). Nothing was written into the caller's
 * planes; width/height/type report the frame's size. get_frame2 holds the
 * frame for the next call; the JPEG decoder drops the image (re-submit). */
#define NVMPI_ERR_FRAME_SIZE (-90)

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
    /* AV1 decode (libnvmpi >= 4.2.0, JetPack 6 only); appended last to keep the ABI. */
    NV_VIDEO_CodingAV1,
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
    int nonblocking;
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

/* nvmpi_fd_frame, NVMPI_ERR_INVAL + ABI asserts (libnvmpi >= 4.1.0) */
#include "nvmpi_fd_frame.h"

/* ------------------------------------------------------------------ */
/* Function pointer typedefs                                           */
/* ------------------------------------------------------------------ */

/* Decoder */
typedef nvmpictx* (*fn_nvmpi_create_decoder)(nvDecParam *param);
typedef int       (*fn_nvmpi_decoder_put_packet)(nvmpictx *ctx, nvPacket *packet);
typedef int       (*fn_nvmpi_decoder_get_frame)(nvmpictx *ctx, nvFrame *frame, bool wait);
/* Capacity-checked copy (libnvmpi >= 4.6.0, optional): payload_size[p] IN =
 * capacity of plane p; returns NVMPI_ERR_FRAME_SIZE when the frame does not
 * fit (jetson-nvmpi#149). */
typedef int       (*fn_nvmpi_decoder_get_frame2)(nvmpictx *ctx, nvFrame *frame, bool wait);
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

/* JPEG decoder DMA-BUF output (libnvmpi >= 4.1.0, optional) */
typedef nvmpictx* (*fn_nvmpi_create_jpeg_decoder_fmt)(nvPixFormat out, int pool_size);
typedef int       (*fn_nvmpi_jpeg_decoder_get_frame_fd)(nvmpictx *ctx,
    nvmpi_fd_frame *frame, nvmpi_frame_release_cb *release,
    void **opaque, bool wait);

/* DMA-BUF fd passthrough (zero-copy) */
typedef int       (*fn_nvmpi_decoder_get_frame_fd)(nvmpictx *ctx,
    int *dmabuf_fd, int *width, int *height, int *pitch,
    int64_t *timestamp, nvmpi_frame_release_cb *release,
    void **opaque, bool wait);
/* Per-plane geometry variant (libnvmpi >= 4.3.0, optional). Prototype copied
 * from jetson-nvmpi v4.3.0 include/nvmpi.h; nvmpi_fd_frame is pinned by
 * the ABI asserts in nvmpi_fd_frame.h. */
typedef int       (*fn_nvmpi_decoder_get_frame_fd2)(nvmpictx *ctx,
    nvmpi_fd_frame *frame, nvmpi_frame_release_cb *release,
    void **opaque, bool wait);
typedef int       (*fn_nvmpi_encoder_put_frame_fd)(nvmpictx *ctx,
    int dmabuf_fd, int width, int height, int pitch,
    int64_t timestamp);
typedef int       (*fn_nvmpi_surface_alloc)(unsigned int width,
    unsigned int height, int *dmabuf_fd);
typedef int       (*fn_nvmpi_surface_destroy)(int dmabuf_fd);

/* Capability bits returned by nvmpi_get_caps() (libnvmpi 4.7.0+). Mirrored
 * from nvmpi.h because FFmpeg builds without the libnvmpi headers. */
#ifndef NVMPI_CAP_DEC_EOS_DRAIN
#define NVMPI_CAP_DEC_EOS_DRAIN (1u << 0)  /* zero-size put_packet drains to the last frame */
#endif
typedef unsigned int (*fn_nvmpi_get_caps)(void);

/* ------------------------------------------------------------------ */
/* Static function pointers (one set per translation unit)             */
/* ------------------------------------------------------------------ */

/* Decoder */
static fn_nvmpi_create_decoder           nvmpi_create_decoder_dl;
static fn_nvmpi_decoder_put_packet       nvmpi_decoder_put_packet_dl;
static fn_nvmpi_decoder_get_frame        nvmpi_decoder_get_frame_dl;
static fn_nvmpi_decoder_get_frame2       nvmpi_decoder_get_frame2_dl;  /* NULL when libnvmpi < 4.6.0 */
static fn_nvmpi_get_caps                 nvmpi_get_caps_dl;            /* NULL when libnvmpi < 4.7.0 */
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

/* JPEG decoder DMA-BUF output: NULL when libnvmpi < 4.1.0 */
static fn_nvmpi_create_jpeg_decoder_fmt    nvmpi_create_jpeg_decoder_fmt_dl;
static fn_nvmpi_jpeg_decoder_get_frame_fd  nvmpi_jpeg_decoder_get_frame_fd_dl;

/* DMA-BUF fd passthrough (zero-copy) */
static fn_nvmpi_decoder_get_frame_fd    nvmpi_decoder_get_frame_fd_dl;
static fn_nvmpi_decoder_get_frame_fd2   nvmpi_decoder_get_frame_fd2_dl;  /* NULL when libnvmpi < 4.3.0 */
static fn_nvmpi_encoder_put_frame_fd    nvmpi_encoder_put_frame_fd_dl;
static fn_nvmpi_surface_alloc           nvmpi_surface_alloc_dl;
static fn_nvmpi_surface_destroy         nvmpi_surface_destroy_dl;

/* Library handle (per-TU; dlopen is refcounted). */
static void *nvmpi_lib_handle;

/* ------------------------------------------------------------------ */
/* Loader                                                              */
/* ------------------------------------------------------------------ */

/* Resolve one required symbol (dlsym + cast) or jump to the fail label. */
#define NVMPI_LOAD_SYM(ptr, name) do {                              \
    *(void **)&(ptr) = dlsym(nvmpi_lib_handle, #name);              \
    if (!(ptr)) goto fail;                                           \
} while (0)

/* Optional symbol (libnvmpi >= 4.1.0 features only): missing -> pointer
 * stays NULL, load continues. Required symbols keep NVMPI_LOAD_SYM (fails
 * loudly). Callers must gate on nvmpi_has_*(). */
#define NVMPI_LOAD_SYM_OPT(ptr, name) do {                          \
    *(void **)&(ptr) = dlsym(nvmpi_lib_handle, #name);              \
} while (0)

/*
 * Load libnvmpi.so.1 (or libnvmpi.so) and resolve all function pointers.
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
    /* Debian runtime package libnvmpi1 ships only the SONAME
     * (libnvmpi.so.1); the unversioned libnvmpi.so symlink is -dev only.
     * Try the SONAME first, fall back to libnvmpi.so for source/dev
     * installs.  On double failure dlerror() reports the last attempt
     * (libnvmpi.so); callers log it. */
    nvmpi_lib_handle = dlopen("libnvmpi.so.1", RTLD_LAZY);
    if (!nvmpi_lib_handle)
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

    /* DMA-BUF fd passthrough symbols (zero-copy) */
    NVMPI_LOAD_SYM(nvmpi_decoder_get_frame_fd_dl,   nvmpi_decoder_get_frame_fd);
    NVMPI_LOAD_SYM(nvmpi_encoder_put_frame_fd_dl,   nvmpi_encoder_put_frame_fd);
    NVMPI_LOAD_SYM(nvmpi_surface_alloc_dl,           nvmpi_surface_alloc);
    NVMPI_LOAD_SYM(nvmpi_surface_destroy_dl,         nvmpi_surface_destroy);

    /* JPEG decoder DMA-BUF output (4.1.0+): optional, 4.0.x keeps loading */
    NVMPI_LOAD_SYM_OPT(nvmpi_create_jpeg_decoder_fmt_dl,   nvmpi_create_jpeg_decoder_fmt);
    NVMPI_LOAD_SYM_OPT(nvmpi_jpeg_decoder_get_frame_fd_dl, nvmpi_jpeg_decoder_get_frame_fd);

    /* Per-plane decoder fd API (4.3.0+): optional, older libraries keep loading */
    NVMPI_LOAD_SYM_OPT(nvmpi_decoder_get_frame_fd2_dl, nvmpi_decoder_get_frame_fd2);

    /* Capacity-checked software-frame copy (4.6.0+): optional so older
     * libraries keep loading; without it the decoders refuse software-frame
     * output (nvmpi_dec_sw.h, jetson-ffmpeg#137). */
    NVMPI_LOAD_SYM_OPT(nvmpi_decoder_get_frame2_dl, nvmpi_decoder_get_frame2);

    /* Capability bits (4.7.0+): optional; a missing symbol means no optional
     * capability, e.g. no decoder EOS drain (nvmpi_dec_drain.h). */
    NVMPI_LOAD_SYM_OPT(nvmpi_get_caps_dl, nvmpi_get_caps);

    return 0;

fail:
    /* A required symbol is missing (ABI mismatch): unload the library. */
    dlclose(nvmpi_lib_handle);
    nvmpi_lib_handle = NULL;
    return -1;
}

/* True when libnvmpi exports the JPEG fd API (both symbols, 4.1.0+).
 * NVMPI_FORCE_NO_JPEG_FD=1 (test hook) fakes a 4.0.x library. */
static inline int nvmpi_has_jpeg_fd(void)
{
    const char *force = getenv("NVMPI_FORCE_NO_JPEG_FD");

    if (force && force[0] == '1')
        return 0;
    return nvmpi_create_jpeg_decoder_fmt_dl && nvmpi_jpeg_decoder_get_frame_fd_dl;
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
#define nvmpi_decoder_get_frame2      nvmpi_decoder_get_frame2_dl
#define nvmpi_get_caps                nvmpi_get_caps_dl
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
#define nvmpi_decoder_get_frame_fd2   nvmpi_decoder_get_frame_fd2_dl
#define nvmpi_encoder_put_frame_fd    nvmpi_encoder_put_frame_fd_dl
#define nvmpi_surface_alloc           nvmpi_surface_alloc_dl
#define nvmpi_surface_destroy         nvmpi_surface_destroy_dl

#endif /* DYNLINK_NVMPI_H */
