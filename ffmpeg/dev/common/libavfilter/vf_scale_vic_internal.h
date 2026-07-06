/*
 * vf_scale_vic_internal.h — shared declarations for the Tegra VIC
 * hardware scale/CSC filter, split across vf_scale_vic.c (filter setup,
 * config, options, registration) and vf_scale_vic_frame.c (per-frame
 * processing).
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

#ifndef AVFILTER_VF_SCALE_VIC_INTERNAL_H
#define AVFILTER_VF_SCALE_VIC_INTERNAL_H

#include "avfilter.h"

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

    /* Source buffer pool: registered NV12 surfaces at input dimensions.
     * The decoder outputs dup'd DMA-BUF fds that are NOT registered in
     * NvBufSurface — NvBufSurfTransform rejects them.  Frame data is
     * copied from the dup'd fd into a source pool buffer before VIC
     * transform.  Ring-buffered like the output pool.
     * Allocated in scale_vic_config_output(); freed in scale_vic_uninit(). */
    int in_fds[VIC_OUTPUT_POOL_SIZE];
    int in_pool_size;
    int in_pool_idx;

    /* Track whether dynlink was loaded */
    int dynlink_loaded;
} ScaleVICContext;

/* Per-frame processing entry point — defined in vf_scale_vic_frame.c,
 * wired into scale_vic_inputs[].filter_frame in vf_scale_vic.c.  External
 * linkage (not static) because it is referenced across translation units. */
int scale_vic_filter_frame(AVFilterLink *inlink, AVFrame *in);

#endif /* AVFILTER_VF_SCALE_VIC_INTERNAL_H */
