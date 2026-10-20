/*
 * nvmpi_dec_fd2.h -- DRM_PRIME frame description for the V4L2 video decoders
 * (h264/hevc/vp8/vp9/mpeg2/mpeg4/av1_nvmpi).
 *
 * libnvmpi >= 4.3.0 exports nvmpi_decoder_get_frame_fd2, which reports the
 * REAL per-plane pitch/offset/size of the borrowed surface. Older libraries
 * only return one pitch, so the chroma offset had to be guessed as
 * pitch*height, which is wrong whenever the allocator page-aligns the luma
 * plane (jetson-ffmpeg#131, jetson-nvmpi#129).
 *
 * Included by nvmpi_dec.c; static inline, not a separate translation unit
 * (kept out of nvmpi_dec.c only to bound its size). The fd2 symbol is
 * OPTIONAL at runtime: a missing symbol selects the legacy pitch*height path
 * with a one-time warning, so no compile-time dependency on libnvmpi 4.3.
 */
#ifndef NVMPI_DEC_FD2_H
#define NVMPI_DEC_FD2_H

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "nvmpi_dec_drm.h"

/* True when libnvmpi exports nvmpi_decoder_get_frame_fd2 (>= 4.3.0).
 * NVMPI_FORCE_NO_FD2=1 (test hook) fakes an older library so the legacy
 * path stays covered by the hw/mock suites. */
static inline int nvmpi_has_decoder_fd2(void)
{
    const char *force = getenv("NVMPI_FORCE_NO_FD2");

    if (force && force[0] == '1')
        return 0;
    return nvmpi_decoder_get_frame_fd2_dl != NULL;
}

/* Fetch one decoded frame as a borrowed DMA-BUF and describe it for
 * nvmpi_drm_wrap_frame().
 *
 * Returns 0 and fills *info (the borrow is then owned by info->release /
 * info->opaque and MUST be handed to nvmpi_drm_wrap_frame, which releases it
 * exactly once on every path), 1 when no frame is ready (nothing borrowed),
 * or a negative AVERROR (the borrow, if any, was already released here). */
static inline int nvmpi_dec_get_drm_info(AVCodecContext *avctx, nvmpictx *ctx,
                                         int wait, NvmpiDRMFrameInfo *info)
{
    nvmpi_frame_release_cb release = NULL;
    void *release_opaque = NULL;
    int res;

    memset(info, 0, sizeof(*info));

    if (nvmpi_has_decoder_fd2()) {
        nvmpi_fd_frame fdf = {0};
        uint64_t chroma_end;

        /* struct_size MUST be set before the call: libnvmpi fills at most
         * that many bytes (forward/backward ABI) and rejects a smaller
         * value without consuming a frame. */
        fdf.struct_size = sizeof(fdf);
        res = nvmpi_decoder_get_frame_fd2_dl(ctx, &fdf, &release,
                                             &release_opaque, wait);
        if (res == NVMPI_ERR_INVAL) {
            av_log(avctx, AV_LOG_ERROR,
                   "nvmpi_decoder_get_frame_fd2 rejected its arguments "
                   "(struct_size/handle mismatch).\n");
            return AVERROR_EXTERNAL;
        }
        if (res < 0)
            return 1;   /* no frame (non-blocking), timeout or shutdown */

        /* The borrow is ours from here: every early exit must release(). */

        /* The DRM descriptor built below is a single NV12 layer. Another
         * format (P010 / YUV420) would be mislabelled and read with wrong
         * strides downstream, so refuse it instead. */
        if (fdf.format != NV_PIX_NV12 || fdf.num_planes != 2) {
            av_log(avctx, AV_LOG_ERROR,
                   "nvmpi: DRM_PRIME output supports NV12 only (format=%d "
                   "planes=%u)\n", (int)fdf.format, fdf.num_planes);
            release(release_opaque);
            return AVERROR_PATCHWELCOME;
        }

        /* Defensive validation of the library-reported layout: a corrupt
         * stream or driver glitch must not make a consumer index outside
         * the dma-buf. The luma plane must fit before the chroma offset and
         * the chroma plane must end inside `size`; sums use 64 bits so
         * pitch*height cannot wrap. */
        chroma_end = (uint64_t)fdf.offset[1] +
                     (uint64_t)fdf.pitch[1] * ((fdf.height + 1) / 2);
        if (fdf.fd < 0 || !fdf.width || !fdf.height ||
            fdf.width > INT_MAX || fdf.height > INT_MAX ||
            fdf.pitch[0] < fdf.width || fdf.pitch[1] < fdf.width ||
            (uint64_t)fdf.offset[0] + (uint64_t)fdf.pitch[0] * fdf.height >
                fdf.offset[1] ||
            chroma_end > fdf.size) {
            av_log(avctx, AV_LOG_ERROR,
                   "nvmpi: inconsistent DMA-BUF layout (%ux%u pitch=%u/%u "
                   "off=%u/%u size=%llu)\n", fdf.width, fdf.height,
                   fdf.pitch[0], fdf.pitch[1], fdf.offset[0], fdf.offset[1],
                   (unsigned long long)fdf.size);
            release(release_opaque);
            return AVERROR_INVALIDDATA;
        }

        info->fd           = fdf.fd;
        /* Original-fd hint: libnvmpi 4.3 keeps the buffer valid until the
         * release callback runs (also after decoder close), so scale_vic and
         * the encoder fd path may use the registered fd and skip the copy.
         * The dup'd fd in the descriptor stays the owning reference. */
        info->orig_fd_hint = 1;
        info->release      = release;
        info->opaque       = release_opaque;
        info->fourcc       = DRM_FORMAT_NV12;
        info->nb_planes    = 2;
        info->pitch[0]     = fdf.pitch[0];
        info->pitch[1]     = fdf.pitch[1];
        info->offset[0]    = fdf.offset[0];
        info->offset[1]    = fdf.offset[1];
        info->size         = (size_t)fdf.size;
        info->width        = (int)fdf.width;
        info->height       = (int)fdf.height;
        info->pts          = fdf.timestamp;
        return 0;
    } else {
        /* Legacy path (libnvmpi < 4.3.0): one pitch only, so the chroma
         * offset is a guess (pitch*height). Wrong when the allocator pads
         * the luma plane; kept only so older libraries keep working. No
         * original-fd hint: before 4.3 the pool is destroyed by decoder
         * close while borrows may be outstanding, so the hint could dangle. */
        static int warned;
        int dmabuf_fd = -1, width = 0, height = 0, pitch = 0;
        int64_t timestamp = 0;

        if (!warned) {
            warned = 1;
            av_log(avctx, AV_LOG_WARNING,
                   "libnvmpi lacks nvmpi_decoder_get_frame_fd2 (needs >= "
                   "4.3.0): deriving DRM_PRIME plane offsets as pitch*height, "
                   "which can be wrong on padded surfaces.\n");
        }
        res = nvmpi_decoder_get_frame_fd(ctx, &dmabuf_fd, &width, &height,
                                         &pitch, &timestamp, &release,
                                         &release_opaque, wait);
        if (res < 0)
            return 1;

        /* Borrow is ours: reject nonsense before it reaches the descriptor
         * (also keeps pitch*height inside 32 bits for offset[1]). */
        if (dmabuf_fd < 0 || width <= 0 || height <= 0 || pitch < width ||
            (uint64_t)pitch * height * 3 / 2 > UINT32_MAX) {
            release(release_opaque);
            return AVERROR_INVALIDDATA;
        }
        info->fd           = dmabuf_fd;
        info->orig_fd_hint = 0;
        info->release      = release;
        info->opaque       = release_opaque;
        info->fourcc       = DRM_FORMAT_NV12;
        info->nb_planes    = 2;
        info->pitch[0]     = pitch;
        info->pitch[1]     = pitch;
        info->offset[1]    = (uint32_t)pitch * height;
        info->size         = (size_t)pitch * height * 3 / 2;
        info->width        = width;
        info->height       = height;
        info->pts          = timestamp;
        return 0;
    }
}

/* Keep avctx and hw_frames_ctx in step with the frame's size (resolution
 * change mid-stream: libnvmpi starts a new pool generation and frames carry
 * the new geometry). Must run BEFORE nvmpi_drm_wrap_frame so the frame refs
 * the matching hw_frames_ctx. On failure releases the borrow in *info. */
static inline int nvmpi_dec_sync_geometry(AVCodecContext *avctx,
                                          const NvmpiDRMFrameInfo *info)
{
    int ret;

    if (info->width == avctx->width && info->height == avctx->height)
        return 0;

    avctx->width  = info->width;
    avctx->height = info->height;
    if (avctx->hw_frames_ctx) {
        /* Older frames keep their own refs to the previous context. */
        av_buffer_unref(&avctx->hw_frames_ctx);
        ret = nvmpi_drm_init_hw_frames(avctx, AV_PIX_FMT_NV12);
        if (ret < 0) {
            info->release(info->opaque);
            return ret;
        }
    }
    return 0;
}

#endif /* NVMPI_DEC_FD2_H */
