/*
 * vf_scale_vic.c — Tegra VIC hardware scale/CSC filter for FFmpeg.
 *
 * Uses the NVIDIA Tegra VIC (Video Image Compositor) engine for
 * hardware-accelerated scaling and color-space conversion on DMA-BUF
 * surfaces. Operates entirely in DRM_PRIME pixel format with zero CPU
 * copies — frames stay in GPU-accessible memory throughout.
 *
 * Pipeline: decoder (DRM_PRIME) → scale_vic → encoder (DRM_PRIME)
 *
 * libnvmpi C API (loaded at runtime via dynlink_nvmpi_vic.h / dlopen).
 * No link-time dependency — if libnvmpi.so is absent the filter is
 * unavailable and FFmpeg continues without it.
 *
 * Usage:
 *   ffmpeg -hwaccel nvmpi -i input.mp4 -vf scale_vic=1280:720 \
 *          -c:v h264_nvmpi output.mp4
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

#include <string.h>
#include <unistd.h>

#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_drm.h"

#include "avfilter.h"
#include "video.h"
#include "formats.h"

/* Version-gated includes:
 *   - filters.h: present in FFmpeg 6.0+ (libavfilter >= 9); contains
 *     FILTER_INPUTS/OUTPUTS macros and FF_FILTER_FLAG_HWFRAME_AWARE.
 *   - internal.h: present up to FFmpeg 7.0; removed in FFmpeg 7.1
 *     (content merged into filters.h).  Both 7.0 and 7.1 share
 *     LIBAVFILTER_VERSION_MAJOR=10 and VERSION_MINOR is unavailable in
 *     internal builds, so __has_include is the reliable check.
 *     GCC 5+ / Clang 3+ support __has_include (all Jetson toolchains). */
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

/* DRM_FORMAT_NV12 from drm_fourcc.h — defined here to avoid a build
 * dependency on libdrm-dev headers.  Same value as the decoder uses
 * in nvmpi_dec.c. */
#ifndef DRM_FORMAT_NV12
#define DRM_FORMAT_NV12 0x3231564E
#endif

/* Runtime-loaded via dlopen — no link-time dependency on libnvmpi.so.
 * Only VIC + surface symbols are resolved (not decoder/encoder). */
#include "dynlink_nvmpi_vic.h"

/* Maximum number of pre-allocated output DMA-BUF surfaces.  Matches
 * the typical decoded-frame pipeline depth — enough for the encoder
 * input queue without over-allocating scarce DMA memory. */
#define VIC_OUTPUT_POOL_SIZE 8

/* ------------------------------------------------------------------ */
/* Filter context                                                      */
/* ------------------------------------------------------------------ */

typedef struct ScaleVICContext {
    const AVClass *class;

    /* User-specified output dimensions (AVOption) */
    int width;
    int height;

    /* VIC engine context — allocated in init, freed in uninit.
     * Allocated in scale_vic_init(); freed in scale_vic_uninit(). */
    nvmpi_vic_ctx *vic_ctx;

    /* Output buffer pool: pre-allocated pitch-linear NV12 DMA-BUF
     * surfaces at the target resolution.  Ring-buffered — pool_idx
     * cycles through the array.
     * Allocated in scale_vic_config_output(); freed in scale_vic_uninit(). */
    int out_fds[VIC_OUTPUT_POOL_SIZE];
    int pool_size;
    int pool_idx;

    /* Track whether dynlink was loaded */
    int dynlink_loaded;
} ScaleVICContext;

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
/* Init / uninit                                                       */
/* ------------------------------------------------------------------ */

static av_cold int scale_vic_init(AVFilterContext *avctx)
{
    ScaleVICContext *s = avctx->priv;
    int i;

    /* Zero-initialize pool fds — marks all slots as unallocated */
    for (i = 0; i < VIC_OUTPUT_POOL_SIZE; i++)
        s->out_fds[i] = -1;
    s->pool_size = 0;
    s->pool_idx  = 0;

    /* Load libnvmpi.so via dlopen on first use */
    if (nvmpi_vic_dynlink_load() < 0) {
        av_log(avctx, AV_LOG_ERROR,
               "scale_vic: failed to load libnvmpi.so: %s\n", dlerror());
        return AVERROR_EXTERNAL;
    }
    s->dynlink_loaded = 1;

    /* Create VIC engine context — binds VIC compute for this thread */
    s->vic_ctx = nvmpi_vic_create();
    if (!s->vic_ctx) {
        av_log(avctx, AV_LOG_ERROR,
               "scale_vic: failed to create VIC context\n");
        return AVERROR_EXTERNAL;
    }

    return 0;
}

static av_cold void scale_vic_uninit(AVFilterContext *avctx)
{
    ScaleVICContext *s = avctx->priv;
    int i;

    /* Destroy output pool surfaces */
    if (s->dynlink_loaded) {
        for (i = 0; i < s->pool_size; i++) {
            if (s->out_fds[i] >= 0) {
                nvmpi_surface_destroy(s->out_fds[i]);
                s->out_fds[i] = -1;
            }
        }

        /* Destroy VIC context — frees VIC session.
         * Freed here; allocated in scale_vic_init(). */
        if (s->vic_ctx) {
            nvmpi_vic_close(s->vic_ctx);
            s->vic_ctx = NULL;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Format negotiation                                                  */
/* ------------------------------------------------------------------ */

/* DRM_PRIME is the only supported format — the VIC operates on DMA-BUF
 * surfaces and cannot accept or produce software pixel data. */
static const enum AVPixelFormat vic_pix_fmts[] = {
    AV_PIX_FMT_DRM_PRIME,
    AV_PIX_FMT_NONE,
};

#if LIBAVFILTER_VERSION_MAJOR < 9
/* FFmpeg 4.x/5.x: query_formats callback on AVFilter.
 * ff_set_common_formats_from_list does not exist before libavfilter 9;
 * use ff_make_format_list + ff_set_common_formats instead. */
static int scale_vic_query_formats(AVFilterContext *avctx)
{
    AVFilterFormats *fmts = ff_make_format_list((const int *)vic_pix_fmts);
    if (!fmts)
        return AVERROR(ENOMEM);
    return ff_set_common_formats(avctx, fmts);
}
#endif

/* ------------------------------------------------------------------ */
/* Output configuration                                                */
/* ------------------------------------------------------------------ */

static int scale_vic_config_output(AVFilterLink *outlink)
{
    AVFilterContext *avctx = outlink->src;
    AVFilterLink *inlink   = avctx->inputs[0];
    ScaleVICContext *s     = avctx->priv;
    int i, ret;

    /* Use explicit dimensions, or pass through input dimensions */
    int out_w = s->width  > 0 ? s->width  : inlink->w;
    int out_h = s->height > 0 ? s->height : inlink->h;

    /* Validate dimensions against Tegra hardware limits */
    if (out_w <= 0 || out_h <= 0 || out_w > 8192 || out_h > 8192) {
        av_log(avctx, AV_LOG_ERROR,
               "scale_vic: invalid output dimensions %dx%d\n", out_w, out_h);
        return AVERROR(EINVAL);
    }

    outlink->w = out_w;
    outlink->h = out_h;

    /* Pre-allocate output DMA-BUF surface pool at target resolution */
    for (i = 0; i < VIC_OUTPUT_POOL_SIZE; i++) {
        /* Allocated here; freed in scale_vic_uninit() via nvmpi_surface_destroy */
        ret = nvmpi_surface_alloc(out_w, out_h, &s->out_fds[i]);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR,
                   "scale_vic: failed to allocate output surface %d (%dx%d)\n",
                   i, out_w, out_h);
            return AVERROR_EXTERNAL;
        }
    }
    s->pool_size = VIC_OUTPUT_POOL_SIZE;
    s->pool_idx  = 0;

    av_log(avctx, AV_LOG_INFO,
           "scale_vic: %dx%d → %dx%d (VIC hardware, %d output buffers)\n",
           inlink->w, inlink->h, out_w, out_h, s->pool_size);

    return 0;
}

/* ------------------------------------------------------------------ */
/* Per-frame transform                                                 */
/* ------------------------------------------------------------------ */

static int scale_vic_filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *avctx = inlink->dst;
    AVFilterLink *outlink  = avctx->outputs[0];
    ScaleVICContext *s     = avctx->priv;
    AVFrame *out = NULL;
    AVDRMFrameDescriptor *in_desc, *out_desc;
    int src_fd, dst_fd, dup_fd, ret;
    int dst_pitch;

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
    src_fd = in_desc->objects[0].fd;

    /* Get next output buffer from the ring pool */
    dst_fd = s->out_fds[s->pool_idx];
    s->pool_idx = (s->pool_idx + 1) % s->pool_size;

    /* Execute VIC hardware transform: scale src → dst */
    ret = nvmpi_vic_transform(s->vic_ctx, src_fd, dst_fd,
                              in->width, in->height,
                              outlink->w, outlink->h);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR,
               "scale_vic: VIC transform failed (%dx%d → %dx%d)\n",
               in->width, in->height, outlink->w, outlink->h);
        av_frame_free(&in);
        return AVERROR_EXTERNAL;
    }

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

    /* NV12 pitch = width aligned to hardware stride. For surfaces allocated
     * by nvmpi_surface_alloc, the driver aligns pitch internally. We compute
     * a conservative estimate matching the decoder's pattern: pitch = width
     * (the actual hardware pitch may be larger, but the DRM descriptor only
     * needs a valid lower bound for downstream consumers). */
    dst_pitch = outlink->w;

    /* Single DMA-BUF object, one NV12 layer with luma + chroma planes */
    out_desc->nb_objects = 1;
    out_desc->objects[0].fd   = dup_fd;
    out_desc->objects[0].size = (size_t)dst_pitch * outlink->h * 3 / 2;

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
    return ff_filter_frame(outlink, out);
}

/* ------------------------------------------------------------------ */
/* AVFilter definition                                                 */
/* ------------------------------------------------------------------ */

#define OFFSET(x) offsetof(ScaleVICContext, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_FILTERING_PARAM

static const AVOption scale_vic_options[] = {
    { "w",      "Output width",  OFFSET(width),  AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 8192, FLAGS },
    { "width",  "Output width",  OFFSET(width),  AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 8192, FLAGS },
    { "h",      "Output height", OFFSET(height), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 8192, FLAGS },
    { "height", "Output height", OFFSET(height), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 8192, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(scale_vic);

static const AVFilterPad scale_vic_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = scale_vic_filter_frame,
    },
#if LIBAVFILTER_VERSION_MAJOR < 9
    { NULL }
#endif
};

static const AVFilterPad scale_vic_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = scale_vic_config_output,
    },
#if LIBAVFILTER_VERSION_MAJOR < 9
    { NULL }
#endif
};

#if LIBAVFILTER_VERSION_MAJOR >= 11
/* FFmpeg 8.0+: FFFilter wraps AVFilter in .p sub-struct.
 * name/description/priv_class live under .p; init/uninit/priv_size
 * are direct FFFilter fields. */
const FFFilter ff_vf_scale_vic = {
    .p.name        = "scale_vic",
    .p.description = NULL_IF_CONFIG_SMALL("Tegra VIC hardware scaler (DRM_PRIME)"),
    .p.priv_class  = &scale_vic_class,

    .init      = scale_vic_init,
    .uninit    = scale_vic_uninit,
    .priv_size = sizeof(ScaleVICContext),

    FILTER_INPUTS(scale_vic_inputs),
    FILTER_OUTPUTS(scale_vic_outputs),
    FILTER_SINGLE_PIXFMT(AV_PIX_FMT_DRM_PRIME),

    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};
#else
const AVFilter ff_vf_scale_vic = {
    .name        = "scale_vic",
    .description = NULL_IF_CONFIG_SMALL("Tegra VIC hardware scaler (DRM_PRIME)"),

    .init   = scale_vic_init,
    .uninit = scale_vic_uninit,

    .priv_size  = sizeof(ScaleVICContext),
    .priv_class = &scale_vic_class,

#if LIBAVFILTER_VERSION_MAJOR >= 9
    /* FFmpeg 6.0+: use FILTER_INPUTS/OUTPUTS macros */
    FILTER_INPUTS(scale_vic_inputs),
    FILTER_OUTPUTS(scale_vic_outputs),
    FILTER_SINGLE_PIXFMT(AV_PIX_FMT_DRM_PRIME),
#else
    /* FFmpeg 4.x/5.x: direct field assignment */
    .inputs  = scale_vic_inputs,
    .outputs = scale_vic_outputs,
    .query_formats = scale_vic_query_formats,
#endif

    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};
#endif
