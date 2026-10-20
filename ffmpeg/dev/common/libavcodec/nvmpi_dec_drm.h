/*
 * nvmpi_dec_drm.h -- DRM_PRIME frame wrapping shared by the nvmpi decoders
 * (nvmpi_dec.c: V4L2 video decoders, nvmpi_dec_mjpeg.c: NvJPEG decoder).
 *
 * Both decoders hand out borrowed libnvmpi DMA-BUF fds as AVFrames. The
 * descriptor / opaque / release logic lives here exactly once. Every
 * function is static inline so each translation unit (patched into FFmpeg
 * and compiled on its own) only pays for what it uses.
 *
 * Include after "dynlink_nvmpi.h" and the libavcodec/libavutil headers
 * that provide AVFrame, AVBufferRef and AVDRMFrameDescriptor.
 */
#ifndef NVMPI_DEC_DRM_H
#define NVMPI_DEC_DRM_H

#include <stdint.h>
#include <unistd.h>

#include "dynlink_nvmpi.h"
#include "libavutil/buffer.h"
#include "libavutil/frame.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_drm.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"

/* DRM_FORMAT constants for AVDRMFrameDescriptor layer description.
 * Defined inline to avoid requiring <drm_fourcc.h> / libdrm-dev as a
 * build dependency -- the constants are stable ABI from the Linux kernel. */
#ifndef DRM_FORMAT_NV12
#define DRM_FORMAT_NV12 \
    (((uint32_t)('N')) | ((uint32_t)('V') << 8) | \
     ((uint32_t)('1') << 16) | ((uint32_t)('2') << 24))
#endif

/* Private DRM format modifier: carries the ORIGINAL NvBufSurface-registered
 * DMA-BUF fd as a hint next to the dup'd fd in objects[0].fd. A dup'd fd is
 * not registered in NvBufSurface's internal table (NvBufSurfaceFromFd fails
 * on it), so a consumer needing a registered surface may decode the hint.
 * The hint is only valid while the frame is referenced AND the decoder that
 * produced it is still open; consumers must fall back to a copy when the
 * lookup fails. Vendor byte 0x4E ("N" for nvmpi) avoids collision with the
 * standard DRM_FORMAT_MOD vendors (0x00-0x0F) and NVIDIA's own (0x03).
 * vf_scale_vic_frame.c writes the same encoding for its output frames. */
#define NVMPI_DRM_MOD_ORIG_FD(fd) \
    ((((uint64_t)0x4E) << 56) | (uint64_t)(unsigned int)(fd))

/* Carries the dup'd DMA-BUF fd and libnvmpi's release callback through to
 * the AVBuffer free path. Allocated per frame; freed in
 * nvmpi_drm_release_frame when FFmpeg drops the last reference to
 * frame->buf[0]. */
typedef struct {
    int fd;                          /* dup'd DMA-BUF fd (owned by the frame) */
    nvmpi_frame_release_cb release;  /* returns the borrow to libnvmpi */
    void *opaque;                    /* libnvmpi release cookie */
} NvmpiDRMOpaque;

/* Everything the wrapper needs to describe one borrowed frame. */
typedef struct {
    int fd;                       /* borrowed libnvmpi fd (never closed here) */
    int orig_fd_hint;             /* 1: publish fd via NVMPI_DRM_MOD_ORIG_FD */
    nvmpi_frame_release_cb release; /* mandatory: called exactly once */
    void *opaque;
    uint32_t fourcc;              /* DRM_FORMAT_* of the single layer */
    int nb_planes;                /* 1..AV_DRM_MAX_PLANES */
    uint32_t pitch[4];            /* bytes per row of each plane */
    uint32_t offset[4];           /* byte offset of each plane in the fd */
    size_t size;                  /* total bytes behind fd */
    int width, height;
    int64_t pts;
} NvmpiDRMFrameInfo;

/* AVBufferRef free callback for DRM_PRIME frames. Runs when FFmpeg drops
 * the last reference to frame->buf[0]. The ORDER is part of the contract
 * (the borrow must outlive our dup, never the reverse):
 *   1. close the dup'd fd (our private copy of the dma-buf reference)
 *   2. call libnvmpi's release callback exactly once
 *   3. free the opaque and the descriptor
 * Single deallocation point for all three; may run on any thread and after
 * the decoder was closed (libnvmpi defers pool destruction to the release). */
static inline void nvmpi_drm_release_frame(void *opaque, uint8_t *data)
{
    NvmpiDRMOpaque *drm = (NvmpiDRMOpaque *)opaque;

    if (drm->fd >= 0)
        close(drm->fd);
    drm->fd = -1;               /* never close twice */

    if (drm->release)
        drm->release(drm->opaque);
    drm->release = NULL;        /* never release twice */

    av_free(drm);
    av_free(data);
}

/* Wrap a borrowed libnvmpi fd into frame (DRM_PRIME).
 *
 * Ownership: on return 0 the frame owns the borrow (released when buf[0] is
 * freed). On ANY error the borrow is released here exactly once and nothing
 * leaks, so callers must not release again. frame->hw_frames_ctx is set from
 * hw_frames_ctx when non-NULL (FFmpeg 7.x derives buffersrc parameters from
 * individual frames; without it the buffersrc rejects the HW pixel format). */
static inline int nvmpi_drm_wrap_frame(AVFrame *frame, const NvmpiDRMFrameInfo *in,
                                       AVBufferRef *hw_frames_ctx)
{
    AVDRMFrameDescriptor *desc = NULL;
    NvmpiDRMOpaque *drm = NULL;
    int dup_fd = -1, i;

    /* Reject a descriptor the AVDRMFrameDescriptor cannot hold before
     * touching any resource; a malformed layout must not index past
     * planes[]. */
    if (!in->release)
        return AVERROR(EINVAL);
    if (in->nb_planes < 1 || in->nb_planes > AV_DRM_MAX_PLANES || in->fd < 0)
        goto fail_einval;

    /* dup(): FFmpeg frame lifetime is unpredictable (filters and encoder
     * queues hold refs, and FFmpeg 8.1+ closes the codec before unref'ing
     * frames), so the frame keeps its own reference to the dma-buf. */
    dup_fd = dup(in->fd);
    if (dup_fd < 0) {
        av_log(NULL, AV_LOG_ERROR, "nvmpi: dup(dmabuf_fd=%d) failed\n", in->fd);
        goto fail_external;
    }

    /* Allocated here; freed in nvmpi_drm_release_frame via av_free(data) */
    desc = av_mallocz(sizeof(*desc));
    /* Allocated here; freed in nvmpi_drm_release_frame via av_free(opaque) */
    drm = av_mallocz(sizeof(*drm));
    if (!desc || !drm)
        goto fail_nomem;

    /* Single DMA-BUF object, one layer. The modifier hint (original fd) is
     * published only when the producer guarantees it stays valid while the
     * frame is referenced (JPEG decoder: refcounted deferred destroy). */
    desc->nb_objects = 1;
    desc->objects[0].fd              = dup_fd;
    desc->objects[0].size            = in->size;
    desc->objects[0].format_modifier =
        in->orig_fd_hint ? NVMPI_DRM_MOD_ORIG_FD(in->fd) : 0;

    desc->nb_layers = 1;
    desc->layers[0].format    = in->fourcc;
    desc->layers[0].nb_planes = in->nb_planes;
    for (i = 0; i < in->nb_planes; i++) {
        desc->layers[0].planes[i].object_index = 0;
        desc->layers[0].planes[i].offset       = in->offset[i];
        desc->layers[0].planes[i].pitch        = in->pitch[i];
    }

    drm->fd      = dup_fd;
    drm->release = in->release;
    drm->opaque  = in->opaque;

    frame->buf[0] = av_buffer_create((uint8_t *)desc, sizeof(*desc),
                                     nvmpi_drm_release_frame, drm,
                                     AV_BUFFER_FLAG_READONLY);
    if (!frame->buf[0])
        goto fail_nomem;
    /* From here nvmpi_drm_release_frame owns dup_fd, the borrow, drm, desc. */

    frame->data[0] = (uint8_t *)desc;
    frame->format  = AV_PIX_FMT_DRM_PRIME;
    frame->width   = in->width;
    frame->height  = in->height;
    frame->pts     = in->pts;
    frame->pkt_dts = AV_NOPTS_VALUE;

    if (hw_frames_ctx) {
        frame->hw_frames_ctx = av_buffer_ref(hw_frames_ctx);
        if (!frame->hw_frames_ctx) {
            /* Unref buf[0]: runs nvmpi_drm_release_frame, which closes the
             * dup'd fd and releases the borrow exactly once. */
            av_buffer_unref(&frame->buf[0]);
            frame->data[0] = NULL;
            return AVERROR(ENOMEM);
        }
    }
    return 0;

fail_nomem:
    /* buf[0] creation failed or an allocation failed: the AVBuffer free
     * callback will never run, so undo everything by hand, in the same
     * order (close dup, release borrow, free). */
    if (dup_fd >= 0)
        close(dup_fd);
    in->release(in->opaque);
    av_free(drm);
    av_free(desc);
    return AVERROR(ENOMEM);
fail_einval:
    in->release(in->opaque);
    return AVERROR(EINVAL);
fail_external:
    in->release(in->opaque);
    return AVERROR_EXTERNAL;
}

/* Create avctx->hw_frames_ctx (DRM_PRIME, sw_format) when an NVMPI hwaccel
 * device is attached and the output format is DRM_PRIME. Tells downstream
 * filters/encoders that frames are DRM_PRIME-wrapped DMA-BUF surfaces.
 * Returns 0 (also when nothing needs to be created) or a negative AVERROR. */
static inline int nvmpi_drm_init_hw_frames(AVCodecContext *avctx,
                                           enum AVPixelFormat sw_format)
{
    AVHWDeviceContext *device_ctx;
    AVHWFramesContext *frames_ctx;
    int ret;

    if (!avctx->hw_device_ctx || avctx->pix_fmt != AV_PIX_FMT_DRM_PRIME)
        return 0;
    device_ctx = (AVHWDeviceContext *)avctx->hw_device_ctx->data;
    if (device_ctx->type != AV_HWDEVICE_TYPE_NVMPI)
        return 0;

    avctx->hw_frames_ctx = av_hwframe_ctx_alloc(avctx->hw_device_ctx);
    if (!avctx->hw_frames_ctx) {
        av_log(avctx, AV_LOG_ERROR, "nvmpi: failed to allocate hw_frames_ctx\n");
        return AVERROR(ENOMEM);
    }
    frames_ctx = (AVHWFramesContext *)avctx->hw_frames_ctx->data;
    frames_ctx->format    = AV_PIX_FMT_DRM_PRIME;
    frames_ctx->sw_format = sw_format;
    frames_ctx->width     = avctx->width;
    frames_ctx->height    = avctx->height;

    ret = av_hwframe_ctx_init(avctx->hw_frames_ctx);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "nvmpi: failed to init hw_frames_ctx: %d\n", ret);
        av_buffer_unref(&avctx->hw_frames_ctx);
        return ret;
    }
    return 0;
}

#endif /* NVMPI_DEC_DRM_H */
