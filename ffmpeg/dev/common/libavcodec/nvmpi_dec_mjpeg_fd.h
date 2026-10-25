/*
 * nvmpi_dec_mjpeg_fd.h -- DRM_PRIME (zero-copy) output of the mjpeg_nvmpi
 * decoder, using libnvmpi 4.1.0's nvmpi_jpeg_decoder_get_frame_fd.
 *
 * Included by nvmpi_dec_mjpeg.c AFTER nvmpiDecodeContext is defined (the
 * functions below are static inline and use that struct). Split out only to
 * keep each file under 400 lines; it is not a separate translation unit.
 *
 * The API is OPTIONAL at runtime: libnvmpi 4.0.x lacks the symbols, so every
 * entry point is gated on nvmpi_has_jpeg_fd() and the caller falls back to
 * the software-frame path (YUV420P) with a warning. No compile-time
 * dependency on libnvmpi 4.1.
 */
#ifndef NVMPI_DEC_MJPEG_FD_H
#define NVMPI_DEC_MJPEG_FD_H

#include <limits.h>

#include "libavutil/time.h"
#include "nvmpi_dec_drm.h"

/* Default budget (ms) for put_packet to wait for a free pool buffer when
 * downstream still holds every borrowed frame. */
#define NVMPI_MJPEG_FD_POOL_WAIT_MS 500

/* Is DRM_PRIME output requested? Explicit only: -output_format drm_prime, a
 * caller-preset avctx->pix_fmt, or an attached NVMPI hwaccel device
 * (-hwaccel nvmpi), the same triggers as the V4L2 video decoders. */
static inline int nvmpi_mjpeg_drm_requested(AVCodecContext *avctx,
                                            const nvmpiDecodeContext *c)
{
    if (c->output_format == AV_PIX_FMT_DRM_PRIME ||
        avctx->pix_fmt == AV_PIX_FMT_DRM_PRIME)
        return 1;
    if (avctx->hw_device_ctx &&
        ((AVHWDeviceContext *)avctx->hw_device_ctx->data)->type ==
            AV_HWDEVICE_TYPE_NVMPI)
        return 1;
    return 0;
}

/* Try to create the fd-output decoder. Returns the handle, or NULL after
 * logging a warning (symbols missing = libnvmpi 4.0.x, or creation failed,
 * e.g. legacy nvbuf_utils build): the caller then uses the software path.
 * NV12 is requested because scale_vic and the DRM_PRIME encoder path consume
 * NV12 (4:2:2 / 4:4:4 JPEGs are converted by the VIC inside libnvmpi). */
static inline nvmpictx *nvmpi_mjpeg_create_fd_decoder(AVCodecContext *avctx,
                                                      const nvmpiDecodeContext *c)
{
    nvmpictx *ctx;

    if (!nvmpi_has_jpeg_fd()) {
        av_log(avctx, AV_LOG_WARNING,
               "drm_prime output requested but libnvmpi lacks the JPEG fd API "
               "(needs >= 4.1.0): falling back to software YUV420P frames.\n");
        return NULL;
    }
    ctx = nvmpi_create_jpeg_decoder_fmt_dl(NV_PIX_NV12, c->frame_pool_size);
    if (!ctx)
        av_log(avctx, AV_LOG_WARNING,
               "libnvmpi could not create the DMA-BUF JPEG decoder: falling "
               "back to software YUV420P frames.\n");
    return ctx;
}

/* Feed one packet; a full pool (every buffer still borrowed downstream) is
 * NOT an error: put_packet returns NVMPI_ERR_EAGAIN without consuming the
 * packet and downstream may release from another thread, so retry briefly.
 * A decode callback must never return AVERROR(EAGAIN) (libavcodec asserts). */
static inline int nvmpi_mjpeg_put_packet_fd(AVCodecContext *avctx,
                                            nvmpiDecodeContext *c, nvPacket *pkt)
{
    int budget = c->wait_timeout > 0 ? c->wait_timeout : NVMPI_MJPEG_FD_POOL_WAIT_MS;
    int res;

    while ((res = nvmpi_jpeg_decoder_put_packet(c->ctx, pkt)) == NVMPI_ERR_EAGAIN) {
        if (budget-- <= 0) {
            av_log(avctx, AV_LOG_ERROR,
                   "DMA-BUF pool exhausted: downstream holds every frame "
                   "(raise -frame_pool_size).\n");
            return AVERROR_EXTERNAL;
        }
        av_usleep(1000);
    }
    if (res < 0) {
        av_log(avctx, AV_LOG_ERROR,
               "nvmpi_jpeg_decoder_put_packet failed (code=%d).\n", res);
        return AVERROR_EXTERNAL;
    }
    return 0;
}

/* .decode callback for the fd-output mode: one JPEG in, one DRM_PRIME frame
 * (NV12 layer, real per-plane pitch/offset from the allocator) out. */
static inline int nvmpi_decode_mjpeg_fd(AVCodecContext *avctx, AVFrame *frame,
                                        int *got_frame, AVPacket *avpkt)
{
    nvmpiDecodeContext *c = avctx->priv_data;
    nvmpi_fd_frame fdf = {0};
    nvmpi_frame_release_cb release = NULL;
    void *release_opaque = NULL;
    NvmpiDRMFrameInfo info = {0};
    nvPacket packet;
    uint64_t chroma_end;
    int res, i;

    *got_frame = 0;
    if (!avpkt->size)
        return 0;

    packet.flags = 0;
    packet.privData = NULL;
    packet.payload_size = avpkt->size;
    packet.payload = avpkt->data;
    packet.pts = avpkt->pts;

    res = nvmpi_mjpeg_put_packet_fd(avctx, c, &packet);
    if (res < 0)
        return res;

    /* struct_size MUST be set before the call: libnvmpi fills at most that
     * many bytes (forward/backward ABI) and rejects a smaller value. */
    fdf.struct_size = sizeof(fdf);
    res = nvmpi_jpeg_decoder_get_frame_fd_dl(c->ctx, &fdf, &release,
                                             &release_opaque, 1);
    if (res == NVMPI_ERR_INVAL) {
        av_log(avctx, AV_LOG_ERROR,
               "nvmpi_jpeg_decoder_get_frame_fd rejected its arguments "
               "(struct_size/handle mismatch).\n");
        return AVERROR_EXTERNAL;
    }
    if (res < 0)
        return avpkt->size;   /* no frame (timeout/EOS), same as the sw path */

    /* From here the borrow is ours: every exit before nvmpi_drm_wrap_frame
     * must call release(), and the wrapper releases on its own errors. */

    /* Defensive validation of the library-reported layout: a hostile or
     * corrupt JPEG must not make a consumer index outside the dma-buf.
     * The chroma plane must end inside `size`; sums use 64 bits. */
    chroma_end = (uint64_t)fdf.offset[1] +
                 (uint64_t)fdf.pitch[1] * ((fdf.height + 1) / 2);
    if (fdf.format != NV_PIX_NV12 || fdf.num_planes != 2 || fdf.fd < 0 ||
        !fdf.width || !fdf.height || fdf.width > INT_MAX || fdf.height > INT_MAX ||
        fdf.pitch[0] < fdf.width || fdf.pitch[1] < fdf.width ||
        fdf.offset[1] < (uint64_t)fdf.pitch[0] * fdf.height ||
        chroma_end > fdf.size) {
        av_log(avctx, AV_LOG_ERROR,
               "nvmpi: inconsistent DMA-BUF JPEG layout (%ux%u planes=%u "
               "pitch=%u/%u off1=%u size=%llu)\n", fdf.width, fdf.height,
               fdf.num_planes, fdf.pitch[0], fdf.pitch[1], fdf.offset[1],
               (unsigned long long)fdf.size);
        release(release_opaque);
        return AVERROR_INVALIDDATA;
    }

    info.fd           = fdf.fd;
    info.orig_fd_hint = 1;  /* libnvmpi keeps the surface alive until release */
    info.release      = release;
    info.opaque       = release_opaque;
    info.fourcc       = DRM_FORMAT_NV12;
    info.nb_planes    = 2;
    for (i = 0; i < 2; i++) {
        info.pitch[i]  = fdf.pitch[i];
        info.offset[i] = fdf.offset[i];
    }
    info.size   = (size_t)fdf.size;
    info.width  = fdf.width;
    info.height = fdf.height;
    info.pts    = fdf.timestamp;

    /* Picture size is self-describing: keep avctx and the hw_frames_ctx in
     * step so downstream filters see consistent geometry. */
    if ((int)fdf.width != avctx->width || (int)fdf.height != avctx->height) {
        avctx->width  = fdf.width;
        avctx->height = fdf.height;
        if (avctx->hw_frames_ctx) {
            av_buffer_unref(&avctx->hw_frames_ctx);
            res = nvmpi_drm_init_hw_frames(avctx, AV_PIX_FMT_NV12);
            if (res < 0) {
                release(release_opaque);
                return res;
            }
        }
    }

    res = nvmpi_drm_wrap_frame(frame, &info, avctx->hw_frames_ctx);
    if (res < 0)
        return res;

    /* Range from the library flag (bit0 = full); applied per frame and to
     * avctx (init already defaulted avctx to JPEG/full). */
    frame->color_range = nvmpi_fd_color_range(fdf.flags, 1);
    avctx->color_range = frame->color_range;

    *got_frame = 1;
    return avpkt->size;
}

#endif /* NVMPI_DEC_MJPEG_FD_H */
