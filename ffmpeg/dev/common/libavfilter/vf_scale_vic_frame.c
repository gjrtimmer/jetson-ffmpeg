/*
 * vf_scale_vic_frame.c — per-frame processing for the Tegra VIC hardware
 * scale/CSC filter.
 *
 * Contains scale_vic_filter_frame() (the hot path: DMA-BUF copy-in, VIC
 * transform dispatch, output frame construction) and its DRM_PRIME frame
 * release callback.  Filter setup, config, options, and registration live
 * in vf_scale_vic.c — see vf_scale_vic_internal.h for the shared
 * ScaleVICContext definition.
 *
 * Copyright (c) 2024-2026 G.J.R. Timmer <gjr.timmer@gmail.com>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2.1 of the License,
 * or (at your option) any later version.
 */

#include <inttypes.h>
#include <unistd.h>

#include "libavutil/mem.h"
#include "libavutil/hwcontext_drm.h"

#include "avfilter.h"
/* ff_filter_frame() lives in internal.h (FFmpeg <9) or filters.h (FFmpeg 9+).
 * Both may coexist; __has_include is supported by all Jetson toolchains. */
#if LIBAVFILTER_VERSION_MAJOR >= 9
#include "filters.h"
#endif
#if defined(__has_include)
#if __has_include("internal.h")
#include "internal.h"
#endif
#else
#include "internal.h"
#endif
#include "video.h"

#include "vf_scale_vic_internal.h"

/* dynlink_nvmpi_vic.h declares all function pointers and
 * nvmpi_vic_dynlink_load() as file-scoped static — each TU that
 * includes it gets its own copy.  vf_scale_vic.c calls
 * nvmpi_vic_dynlink_load() in its .init, but that only populates
 * vf_scale_vic.c's copy.  This TU's pointers remain NULL unless we
 * call nvmpi_vic_dynlink_load() ourselves.  The function is
 * idempotent per-TU (checks nvmpi_vic_lib_handle). */

/* DRM_FORMAT_NV12 from drm_fourcc.h — defined here to avoid a build
 * dependency on libdrm-dev headers.  Same value as the decoder uses
 * in nvmpi_dec.c. */
#ifndef DRM_FORMAT_NV12
#define DRM_FORMAT_NV12 0x3231564E
#endif

/* ------------------------------------------------------------------ */
/* Release callback for output DRM_PRIME frames                        */
/* ------------------------------------------------------------------ */

/* The output DMA-BUF fd is borrowed from the pool, not dup'd — but
 * FFmpeg filters may hold frame refs across multiple filter_frame calls.
 * We dup() the fd so each output frame owns its copy. This callback
 * closes the dup'd fd when the frame ref is released. */
static void scale_vic_drm_release(void *opaque, uint8_t *data)
{
    AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor *)data;
    if (desc && desc->nb_objects > 0 && desc->objects[0].fd >= 0)
        close(desc->objects[0].fd);
    av_free(desc);
}

/* ------------------------------------------------------------------ */
/* Per-frame transform                                                 */
/* ------------------------------------------------------------------ */

int scale_vic_filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    nvmpi_vic_dynlink_load(); /* populate this TU's static function pointers */
    AVFilterContext *avctx = inlink->dst;
    AVFilterLink *outlink  = avctx->outputs[0];
    ScaleVICContext *s     = avctx->priv;
    AVFrame *out = NULL;
    AVDRMFrameDescriptor *in_desc, *out_desc;
    int src_fd, dst_fd, dup_fd, ret;
    int dst_pitch;

    av_log(avctx, AV_LOG_DEBUG,
           "scale_vic: filter_frame entry pts=%" PRId64 " %dx%d fmt=%d\n",
           in->pts, in->width, in->height, in->format);

    /* Extract source DMA-BUF fd from input DRM_PRIME frame */
    if (in->format != AV_PIX_FMT_DRM_PRIME) {
        av_log(avctx, AV_LOG_ERROR,
               "scale_vic: input frame is not DRM_PRIME (format=%d)\n",
               in->format);
        av_frame_free(&in);
        return AVERROR(EINVAL);
    }

    in_desc = (AVDRMFrameDescriptor *)in->data[0];
    if (!in_desc || in_desc->nb_objects < 1) {
        av_log(avctx, AV_LOG_ERROR,
               "scale_vic: input frame has no DRM objects\n");
        av_frame_free(&in);
        return AVERROR(EINVAL);
    }

    av_log(avctx, AV_LOG_DEBUG,
           "scale_vic: input fd=%d pitch=%d, %dx%d → %dx%d\n",
           in_desc->objects[0].fd,
           (int)in_desc->layers[0].planes[0].pitch,
           in->width, in->height, outlink->w, outlink->h);

    /* Copy the dup'd DMA-BUF frame data into a registered source buffer.
     * The decoder dup()s its fds for AVFrame lifetime safety, but dup'd
     * fds are not registered in NvBufSurface's internal table — the VIC
     * hardware transform API (NvBufSurfTransform) requires registered
     * surfaces.  This copy bridges that gap at the cost of one CPU memcpy
     * per frame (~1.4 MB for 720p NV12). */
    src_fd = s->in_fds[s->in_pool_idx];
    s->in_pool_idx = (s->in_pool_idx + 1) % s->in_pool_size;

    ret = nvmpi_surface_copy_from_dmabuf(src_fd,
                                         in_desc->objects[0].fd,
                                         in->width, in->height,
                                         in_desc->layers[0].planes[0].pitch);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR,
               "scale_vic: failed to copy input frame to registered "
               "surface\n");
        av_frame_free(&in);
        return AVERROR_EXTERNAL;
    }

    if (in->width == outlink->w && in->height == outlink->h) {
        /* Passthrough: same dimensions — skip VIC transform entirely.
         * The registered source buffer already contains the NV12 data;
         * use it directly as the output.  This avoids the NvBufSurfTransform
         * identity deadlock (JP6 VIC hangs on same-format same-dimensions)
         * and eliminates bBlitMode in the downstream MMAP encoder. */
        dst_fd = src_fd;
        dst_pitch = in_desc->layers[0].planes[0].pitch;
        av_log(avctx, AV_LOG_DEBUG,
               "scale_vic: passthrough %dx%d (CPU copy, no VIC)\n",
               in->width, in->height);
    } else {
        /* Scaling: use VIC hardware transform NV12 → NV12 at different
         * dimensions.  No identity deadlock because dims differ. */
        dst_fd = s->out_fds[s->pool_idx];
        s->pool_idx = (s->pool_idx + 1) % s->pool_size;

        av_log(avctx, AV_LOG_DEBUG,
               "scale_vic: transform src_fd=%d dst_fd=%d "
               "(%dx%d -> %dx%d)\n",
               src_fd, dst_fd,
               in->width, in->height, outlink->w, outlink->h);

        ret = nvmpi_vic_transform(s->vic_ctx, src_fd, dst_fd,
                                  in->width, in->height,
                                  outlink->w, outlink->h);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR,
                   "scale_vic: VIC transform failed (%dx%d -> %dx%d)\n",
                   in->width, in->height, outlink->w, outlink->h);
            av_frame_free(&in);
            return AVERROR_EXTERNAL;
        }

        dst_pitch = outlink->w;
    }

    av_log(avctx, AV_LOG_DEBUG,
           "scale_vic: transform done, building output frame\n");

    /* Build output frame */
    out = av_frame_alloc();
    if (!out) {
        av_frame_free(&in);
        return AVERROR(ENOMEM);
    }

    /* dup() the pool fd — the output frame may outlive the next filter_frame
     * call (encoder queues, filter chains), so it needs its own fd copy.
     * The dup'd fd is closed in scale_vic_drm_release when the last buf[0]
     * ref drops. */
    dup_fd = dup(dst_fd);
    if (dup_fd < 0) {
        av_log(avctx, AV_LOG_ERROR,
               "scale_vic: dup(fd=%d) failed\n", dst_fd);
        av_frame_free(&out);
        av_frame_free(&in);
        return AVERROR_EXTERNAL;
    }

    /* Allocated here; freed in scale_vic_drm_release via av_free(data) */
    out_desc = av_mallocz(sizeof(*out_desc));
    if (!out_desc) {
        close(dup_fd);
        av_frame_free(&out);
        av_frame_free(&in);
        return AVERROR(ENOMEM);
    }

    /* NV12 pitch: for scaling cases, the output surface uses outlink->w.
     * For passthrough, dst_pitch was set from the input descriptor's pitch
     * (matching the registered source surface).  Both branches above have
     * already set dst_pitch — do not overwrite here. */

    /* Single DMA-BUF object, one NV12 layer with luma + chroma planes.
     * format_modifier carries the original NvBufSurface-registered fd
     * (dst_fd) so the downstream encoder can call NvBufSurfaceFromFd /
     * V4L2 DMABUF qbuf without hitting the dup'd-fd lookup failure.
     * Uses the same NVMPI_DRM_MOD_ORIG_FD convention as the decoder. */
    out_desc->nb_objects = 1;
    out_desc->objects[0].fd              = dup_fd;
    /* Widen both operands to size_t before multiply to prevent
     * int32 overflow at 8192x8192+ (dst_pitch * h wraps int). */
    out_desc->objects[0].size            = (size_t)dst_pitch * (size_t)outlink->h * 3 / 2;
    out_desc->objects[0].format_modifier =
        ((0x4EULL) << 56) | ((uint64_t)(unsigned int)(dst_fd));

    out_desc->nb_layers = 1;
    out_desc->layers[0].format    = DRM_FORMAT_NV12;
    out_desc->layers[0].nb_planes = 2;
    /* Luma plane: offset 0, full stride */
    out_desc->layers[0].planes[0].object_index = 0;
    out_desc->layers[0].planes[0].offset       = 0;
    out_desc->layers[0].planes[0].pitch        = dst_pitch;
    /* Chroma plane: after luma, same stride (NV12 interleaved UV) */
    out_desc->layers[0].planes[1].object_index = 0;
    out_desc->layers[0].planes[1].offset       = (ptrdiff_t)dst_pitch * outlink->h;
    out_desc->layers[0].planes[1].pitch        = dst_pitch;

    out->data[0] = (uint8_t *)out_desc;
    out->buf[0]  = av_buffer_create((uint8_t *)out_desc, sizeof(*out_desc),
                                     scale_vic_drm_release, NULL,
                                     AV_BUFFER_FLAG_READONLY);
    if (!out->buf[0]) {
        close(dup_fd);
        av_free(out_desc);
        av_frame_free(&out);
        av_frame_free(&in);
        return AVERROR(ENOMEM);
    }

    out->format = AV_PIX_FMT_DRM_PRIME;
    out->width  = outlink->w;
    out->height = outlink->h;

    /* Copy timing metadata from input frame */
    out->pts     = in->pts;
    out->pkt_dts = in->pkt_dts;
    /* AVFrame.duration exists in all supported FFmpeg versions (6.0+).
     * pkt_duration was deprecated in 6.0 and removed in 7.0+. */
    out->duration = in->duration;
    out->time_base      = in->time_base;
    out->sample_aspect_ratio = in->sample_aspect_ratio;
    out->colorspace      = in->colorspace;
    out->color_range     = in->color_range;
    out->color_primaries = in->color_primaries;
    out->color_trc       = in->color_trc;

    av_frame_free(&in);

    av_log(avctx, AV_LOG_DEBUG,
           "scale_vic: calling ff_filter_frame (output %dx%d, fd=%d)\n",
           out->width, out->height, dup_fd);

    return ff_filter_frame(outlink, out);
}
