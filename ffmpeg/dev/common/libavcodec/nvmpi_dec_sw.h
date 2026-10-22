/*
 * nvmpi_dec_sw.h -- software-frame (copy) output for the V4L2 video decoders
 * (h264/hevc/vp8/vp9/mpeg2/mpeg4/av1_nvmpi).
 *
 * libnvmpi copies each decoded frame into FFmpeg-owned planes (bufFrame).
 * Before libnvmpi 4.6.0 the copying get_frame knew neither the capacity of
 * those planes nor told the caller the frame geometry, so a mid-stream
 * resolution INCREASE overflowed the heap buffer (jetson-ffmpeg#137,
 * jetson-nvmpi#149). libnvmpi >= 4.6.0 exports nvmpi_decoder_get_frame2: the
 * caller passes every plane's capacity, and a frame that does not fit is
 * refused (NVMPI_ERR_FRAME_SIZE), kept pending and reported with its size.
 *
 * Included by nvmpi_dec.c; static inline, not a separate translation unit
 * (kept out of nvmpi_dec.c only to bound its size).
 */
#ifndef NVMPI_DEC_SW_H
#define NVMPI_DEC_SW_H

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>

#include "libavutil/frame.h"
#include "libavutil/imgutils.h"
#include "libavutil/pixdesc.h"

/* True when libnvmpi exports nvmpi_decoder_get_frame2 (>= 4.6.0).
 * NVMPI_FORCE_NO_GET_FRAME2=1 (test hook) fakes an older library so the
 * init-time refusal stays covered by the mock/hw suites. */
static inline int nvmpi_has_decoder_get_frame2(void)
{
    const char *force = getenv("NVMPI_FORCE_NO_GET_FRAME2");

    if (force && force[0] == '1')
        return 0;
    return nvmpi_decoder_get_frame2_dl != NULL;
}

/* Init-time gate for software-frame output (pix_fmt != DRM_PRIME).
 *
 * Also gates the mjpeg_nvmpi software path: its capacity-checked JPEG copy
 * ships in the same libnvmpi release, so get_frame2 marks it as present.
 *
 * Without get_frame2 the copy cannot be bounded: the old get_frame writes
 * the decoded geometry into the planes without knowing their size, and the
 * only call that reports geometry first (get_frame_fd2) consumes the frame,
 * so a size increase cannot be detected before the overflow. Software
 * frames are therefore refused on libnvmpi < 4.6.0 instead of risking heap
 * corruption on a crafted or resolution-switching stream. DRM_PRIME output
 * (-hwaccel nvmpi) copies nothing and keeps working on older libraries. */
static inline int nvmpi_dec_sw_check_support(AVCodecContext *avctx)
{
    if (nvmpi_has_decoder_get_frame2())
        return 0;
    av_log(avctx, AV_LOG_ERROR,
           "libnvmpi lacks nvmpi_decoder_get_frame2 (needs >= 4.6.0): "
           "software-frame output cannot bound the frame copy and is "
           "disabled (jetson-ffmpeg#137). Upgrade libnvmpi, or use DRM_PRIME "
           "output (-hwaccel nvmpi or -output_format drm_prime).\n");
    return AVERROR(ENOSYS);
}

/* Capacity in bytes of plane p of f: from data[p] to the end of the
 * AVBufferRef that holds it, clamped at the start of any later plane in the
 * same buffer (custom get_buffer2 callbacks may pack all planes into one
 * buffer; the clamp keeps one plane's copy out of its neighbour). Returns 0
 * when no buffer holds data[p]: the capacity is then unknown and the caller
 * must refuse the copy. Pointer math is done on uintptr_t so a plane outside
 * a buffer is never turned into a negative/garbage size. */
static inline size_t nvmpi_dec_sw_plane_cap(const AVFrame *f, int p, int nb_planes)
{
    uintptr_t start = (uintptr_t)f->data[p];
    size_t cap = 0;
    int i;

    if (!f->data[p])
        return 0;
    for (i = 0; i < AV_NUM_DATA_POINTERS && f->buf[i]; i++) {
        uintptr_t lo = (uintptr_t)f->buf[i]->data;
        uintptr_t hi = lo + f->buf[i]->size;

        /* hi < lo would mean a wrapped buffer range: never trust it. */
        if (hi >= lo && start >= lo && start < hi) {
            cap = hi - start;
            break;
        }
    }
    if (!cap)
        return 0;
    for (i = 0; i < nb_planes; i++) {
        uintptr_t q = (uintptr_t)f->data[i];

        if (i != p && q > start && q - start < cap)
            cap = q - start;
    }
    return cap;
}

/* Point nf at bufFrame's planes and pass each plane's capacity (get_frame2
 * contract: payload non-NULL and payload_size non-zero for every plane of
 * the format). Returns 0 or AVERROR_BUG when a capacity is unknown. */
static inline int nvmpi_dec_sw_bind(AVCodecContext *avctx, const AVFrame *f,
                                    nvFrame *nf)
{
    int nb_planes = av_pix_fmt_count_planes(f->format);
    int p;

    /* nvFrame carries at most three planes (YUV420P 3, NV12/P010 2). */
    if (nb_planes <= 0 || nb_planes > 3)
        return AVERROR_BUG;
    for (p = 0; p < nb_planes; p++) {
        size_t cap = nvmpi_dec_sw_plane_cap(f, p, nb_planes);

        /* Negative linesizes (bottom-up layouts) cannot be expressed in the
         * unsigned nvFrame linesize; an unknown capacity cannot be bounded. */
        if (!cap || f->linesize[p] <= 0) {
            av_log(avctx, AV_LOG_ERROR,
                   "nvmpi: cannot bound the frame copy (plane %d: cap=%zu "
                   "linesize=%d)\n", p, cap, f->linesize[p]);
            return AVERROR_BUG;
        }
        nf->payload[p]      = f->data[p];
        nf->linesize[p]     = (unsigned int)f->linesize[p];
        nf->payload_size[p] = cap;   /* size_t and unsigned long: 64 bit on aarch64 */
    }
    return 0;
}

/* (Re)acquire bufFrame at the current avctx size. ff_get_buffer() refuses
 * a frame that still holds data, so any old buffer is dropped first. */
static inline int nvmpi_dec_sw_alloc(AVCodecContext *avctx, AVFrame *f)
{
    av_frame_unref(f);
    f->width  = avctx->width;
    f->height = avctx->height;
    f->format = avctx->pix_fmt;
    return ff_get_buffer(avctx, f, 0);
}

/* Adopt the size libnvmpi reported for the decoded frame. The size comes
 * from the (untrusted) bitstream, so it is checked against FFmpeg's image
 * limits and avctx->max_pixels BEFORE avctx is touched (ff_set_dimensions
 * zeroes avctx's size on failure, which would wedge later allocations). */
static inline int nvmpi_dec_sw_set_size(AVCodecContext *avctx,
                                        unsigned int w, unsigned int h)
{
    if (!w || !h || w > INT_MAX || h > INT_MAX ||
        av_image_check_size2(w, h, avctx->max_pixels, avctx->pix_fmt, 0,
                             avctx) < 0) {
        av_log(avctx, AV_LOG_ERROR, "nvmpi: invalid decoded size %ux%u\n", w, h);
        return AVERROR_INVALIDDATA;
    }
    if ((int)w == avctx->width && (int)h == avctx->height)
        return 0;
    av_log(avctx, AV_LOG_VERBOSE, "nvmpi: frame size %dx%d -> %ux%u\n",
           avctx->width, avctx->height, w, h);
    return ff_set_dimensions(avctx, (int)w, (int)h);
}

/* Drop the frame libnvmpi keeps pending after NVMPI_ERR_FRAME_SIZE when it
 * cannot be taken (size invalid, or still refused after re-allocating).
 * Without this every later call would hit the same frame and fail again, so
 * decoding could never move past it. get_frame_fd2 returns the pending frame
 * first; borrowing and releasing it at once discards it. */
static inline void nvmpi_dec_sw_drop_pending(AVCodecContext *avctx, nvmpictx *ctx)
{
    nvmpi_frame_release_cb release = NULL;
    void *opaque = NULL;
    nvmpi_fd_frame fdf = {0};

    if (!nvmpi_decoder_get_frame_fd2_dl)
        return;
    /* struct_size before the call: libnvmpi fills at most that many bytes. */
    fdf.struct_size = sizeof(fdf);
    if (nvmpi_decoder_get_frame_fd2_dl(ctx, &fdf, &release, &opaque, false) == 0 &&
        release) {
        release(opaque);
        av_log(avctx, AV_LOG_WARNING, "nvmpi: dropped a frame that cannot be output.\n");
    }
}

/* Copy ONE decoded frame into bufFrame (allocated by the caller).
 * Returns 0 when bufFrame holds the frame (its width/height set to the
 * decoded size), 1 when no frame is ready, or a negative AVERROR.
 * On NVMPI_ERR_FRAME_SIZE (frame larger than bufFrame, kept pending by
 * libnvmpi) avctx and bufFrame are resized and the call retried ONCE; the
 * pending frame is returned first, so no frame is lost. */
static inline int nvmpi_dec_sw_get_frame(AVCodecContext *avctx, nvmpictx *ctx,
                                         AVFrame *bufFrame, int wait,
                                         int64_t *pts)
{
    int attempt, res;

    for (attempt = 0; attempt < 2; attempt++) {
        nvFrame nf = {0};

        res = nvmpi_dec_sw_bind(avctx, bufFrame, &nf);
        if (res < 0)
            return res;
        res = nvmpi_decoder_get_frame2(ctx, &nf, wait);
        if (res == 0) {
            /* The frame fit; it may be smaller than bufFrame (shrink): the
             * output frame and avctx carry the decoded size. */
            res = nvmpi_dec_sw_set_size(avctx, nf.width, nf.height);
            if (res < 0)
                return res;
            bufFrame->width  = (int)nf.width;
            bufFrame->height = (int)nf.height;
            *pts = nf.timestamp;
            return 0;
        }
        if (res != NVMPI_ERR_FRAME_SIZE)
            break;
        /* Frame does not fit (resolution increase): nothing was copied and
         * libnvmpi keeps it pending. A second refusal after re-allocating
         * at the reported size would mean the size report is inconsistent:
         * fail instead of looping. */
        if (attempt)
            break;
        res = nvmpi_dec_sw_set_size(avctx, nf.width, nf.height);
        if (res < 0) {
            nvmpi_dec_sw_drop_pending(avctx, ctx);
            return res;
        }
        /* Allocation failure keeps the frame pending: the next call
         * re-allocates at the (already updated) avctx size and takes it. */
        res = nvmpi_dec_sw_alloc(avctx, bufFrame);
        if (res < 0)
            return res;
    }

    if (res == NVMPI_ERR_FRAME_SIZE) {
        av_log(avctx, AV_LOG_ERROR,
               "nvmpi_decoder_get_frame2: frame still does not fit after "
               "re-allocating at %dx%d.\n", avctx->width, avctx->height);
        nvmpi_dec_sw_drop_pending(avctx, ctx);
        return AVERROR_EXTERNAL;
    }
    if (res == NVMPI_ERR_INVAL) {
        av_log(avctx, AV_LOG_ERROR,
               "nvmpi_decoder_get_frame2 rejected its arguments.\n");
        return AVERROR_EXTERNAL;
    }
    if (res < 0)
        return 1;   /* no decoded frame ready (or bounded wait timed out) */
    /* res > 0: a frame was dequeued but the CPU map for the copy failed; the
     * frame is lost and bufFrame holds undefined pixels. */
    av_log(avctx, AV_LOG_ERROR, "nvmpi_decoder_get_frame2: buffer map failed (%d).\n", res);
    return AVERROR_EXTERNAL;
}

#endif /* NVMPI_DEC_SW_H */
