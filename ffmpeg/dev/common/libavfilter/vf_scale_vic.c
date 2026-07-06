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
 * This file holds filter setup, config, options, and registration.
 * Per-frame processing (scale_vic_filter_frame + the DRM_PRIME release
 * callback) lives in vf_scale_vic_frame.c — see vf_scale_vic_internal.h
 * for the shared ScaleVICContext definition.
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

#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/hwcontext.h"

#include "avfilter.h"
#include "video.h"
#include "formats.h"

/* avfilter.h only includes version_major.h during internal FFmpeg builds
 * (HAVE_AV_CONFIG_H is defined) — version.h is intentionally skipped to
 * reduce rebuild scope.  We need LIBAVFILTER_VERSION_MINOR for the
 * hw_frames_ctx location guard (AVFilterLink vs FilterLink at 10.5+),
 * so pull it in explicitly.  Present in all supported versions (6.0–8.1);
 * __has_include guard for safety against future restructuring. */
#if defined(__has_include)
#if __has_include("libavfilter/version.h")
#include "libavfilter/version.h"
#endif
#else
#include "libavfilter/version.h"
#endif

/* Version-gated includes:
 *   - filters.h: present in FFmpeg 6.0+ (libavfilter >= 9); contains
 *     FILTER_INPUTS/OUTPUTS macros, FilterLink, and ff_filter_link().
 *   - internal.h: present up to FFmpeg 7.0; removed in FFmpeg 7.1
 *     (content merged into filters.h).  Both 7.0 and 7.1 share
 *     LIBAVFILTER_VERSION_MAJOR=10 and VERSION_MINOR distinguishes them,
 *     but internal.h detection uses __has_include as the reliable check.
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

/* Runtime-loaded via dlopen — no link-time dependency on libnvmpi.so.
 * Only VIC + surface symbols are resolved (not decoder/encoder).
 * Shared ScaleVICContext + scale_vic_filter_frame() declaration; the
 * per-frame implementation lives in vf_scale_vic_frame.c. */
#include "vf_scale_vic_internal.h"

/* ------------------------------------------------------------------ */
/* Init / uninit                                                       */
/* ------------------------------------------------------------------ */

static av_cold int scale_vic_init(AVFilterContext *avctx)
{
    ScaleVICContext *s = avctx->priv;
    int i;

    /* Zero-initialize pool fds — marks all slots as unallocated */
    for (i = 0; i < VIC_OUTPUT_POOL_SIZE; i++) {
        s->out_fds[i] = -1;
        s->in_fds[i]  = -1;
    }
    s->pool_size    = 0;
    s->pool_idx     = 0;
    s->in_pool_size = 0;
    s->in_pool_idx  = 0;

    /* Load libnvmpi.so via dlopen on first use */
    if (nvmpi_vic_dynlink_load() < 0) {
        av_log(avctx, AV_LOG_ERROR,
               "scale_vic: failed to load libnvmpi.so: %s\n", dlerror());
        return AVERROR_EXTERNAL;
    }
    s->dynlink_loaded = 1;

    av_log(avctx, AV_LOG_INFO,
           "scale_vic: init — dynlink loaded, creating VIC context\n");

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

    if (s->dynlink_loaded) {
        /* Destroy output pool surfaces */
        for (i = 0; i < s->pool_size; i++) {
            if (s->out_fds[i] >= 0) {
                nvmpi_surface_destroy(s->out_fds[i]);
                s->out_fds[i] = -1;
            }
        }

        /* Destroy source pool surfaces */
        for (i = 0; i < s->in_pool_size; i++) {
            if (s->in_fds[i] >= 0) {
                nvmpi_surface_destroy(s->in_fds[i]);
                s->in_fds[i] = -1;
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

    /* Propagate the hardware frames context from the decoder's output
     * to this filter's output.  Without this, FFmpeg's filter graph
     * does not know the output is DRM_PRIME hardware frames and may
     * insert an automatic format converter that tries to read the
     * DRM_PRIME AVDRMFrameDescriptor as raw pixel data — hanging the
     * pipeline in a kernel DMA-BUF read.  All FFmpeg hardware filters
     * (scale_vaapi, scale_cuda, scale_npp) propagate hw_frames_ctx;
     * omitting it is the root cause of the filter pipeline hang.
     *
     * FFmpeg 7.1+ (libavfilter 10.5+): hw_frames_ctx moved from
     * AVFilterLink to FilterLink (accessed via ff_filter_link()).
     * Earlier versions (6.0–7.0) have it directly on AVFilterLink.
     *
     * Unref any old hw_frames_ctx first (reinit safety — FFmpeg 7.x
     * can call config_output a second time). */
#if (LIBAVFILTER_VERSION_MAJOR > 10) || \
    (LIBAVFILTER_VERSION_MAJOR == 10 && LIBAVFILTER_VERSION_MINOR >= 5)
    {
        FilterLink *il = ff_filter_link(inlink);
        FilterLink *ol = ff_filter_link(outlink);
        av_buffer_unref(&ol->hw_frames_ctx);
        if (il->hw_frames_ctx) {
            ol->hw_frames_ctx = av_buffer_ref(il->hw_frames_ctx);
            if (!ol->hw_frames_ctx)
                return AVERROR(ENOMEM);
        }
    }
#else
    av_buffer_unref(&outlink->hw_frames_ctx);
    if (inlink->hw_frames_ctx) {
        outlink->hw_frames_ctx = av_buffer_ref(inlink->hw_frames_ctx);
        if (!outlink->hw_frames_ctx)
            return AVERROR(ENOMEM);
    }
#endif

    /* Surface pool allocation.
     *
     * FFmpeg 7.x re-derives buffersrc parameters from decoded frames,
     * which can trigger config_output a second time (filter reinit).
     * The existing surface pools are still valid — FFmpeg does NOT call
     * uninit during reinit.  Freeing and reallocating DMA-BUF surfaces
     * corrupts the Tegra NvBufSurface fd-to-surface cache, causing
     * NvMapMemCacheMaint errors and VIC transform hangs.
     *
     * Skip pool reallocation if pools are already allocated — the reinit
     * is for format/hw_frames_ctx propagation only. */
    if (s->in_pool_size > 0) {
        av_log(avctx, AV_LOG_INFO,
               "scale_vic: config_output reinit — reusing %d+%d existing "
               "surface pools\n", s->in_pool_size, s->pool_size);
    } else {
        /* First call — allocate source DMA-BUF surface pool at input
         * resolution.  The decoder outputs dup'd fds that are not
         * registered in NvBufSurface.  Frame data is copied into these
         * registered buffers before VIC transform. */
        /* Increment pool counter per successful allocation so uninit
         * can free fds 0..size-1 on partial failure.  Previously the
         * counter was set only after the loop, leaving partially
         * allocated fds leaked on mid-loop failure. */
        s->in_pool_size = 0;
        for (i = 0; i < VIC_OUTPUT_POOL_SIZE; i++) {
            /* Allocated here; freed in scale_vic_uninit() */
            ret = nvmpi_surface_alloc(inlink->w, inlink->h, &s->in_fds[i]);
            if (ret < 0) {
                av_log(avctx, AV_LOG_ERROR,
                       "scale_vic: failed to allocate input surface %d "
                       "(%dx%d)\n", i, inlink->w, inlink->h);
                return AVERROR_EXTERNAL;
            }
            s->in_pool_size++;
        }
        s->in_pool_idx  = 0;

        /* Pre-allocate output DMA-BUF surface pool at target resolution.
         * Uses VIDEO_ENC memtag — these surfaces are VIC transform
         * destinations AND encoder DMABUF inputs; the encoder's NvMMLite
         * layer requires VIDEO_ENC to resolve NvMap handles during its
         * internal format conversion. */
        s->pool_size = 0;
        for (i = 0; i < VIC_OUTPUT_POOL_SIZE; i++) {
            /* Allocated here; freed in scale_vic_uninit() */
            ret = nvmpi_surface_alloc_for_enc(out_w, out_h, &s->out_fds[i]);
            if (ret < 0) {
                av_log(avctx, AV_LOG_ERROR,
                       "scale_vic: failed to allocate output surface %d "
                       "(%dx%d)\n", i, out_w, out_h);
                return AVERROR_EXTERNAL;
            }
            s->pool_size++;
        }
        s->pool_idx  = 0;
    }

    av_log(avctx, AV_LOG_INFO,
           "scale_vic: %dx%d → %dx%d (hardware, %d source + %d output buffers)\n",
           inlink->w, inlink->h, out_w, out_h,
           s->in_pool_size, s->pool_size);

    return 0;
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
