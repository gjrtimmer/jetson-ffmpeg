/*
 * hwcontext_nvmpi.h — FFmpeg hardware device context for NVIDIA Jetson
 * NVMPI (V4L2 M2M) decode/encode via libnvmpi.
 *
 * This file is patched into a vanilla FFmpeg source tree (libavutil/)
 * by scripts/ffpatch.sh. It registers AV_HWDEVICE_TYPE_NVMPI so that
 * -hwaccel nvmpi works from the ffmpeg command line.
 *
 * No NVIDIA-specific headers are required — libnvmpi handles all V4L2
 * and DMA-BUF management internally. The device context is intentionally
 * minimal: it serves as a type marker for format negotiation, not as a
 * resource manager.
 */

#ifndef AVUTIL_HWCONTEXT_NVMPI_H
#define AVUTIL_HWCONTEXT_NVMPI_H

/**
 * NVMPI device context.
 *
 * Allocated as AVHWDeviceContext.hwctx. Empty because libnvmpi manages
 * V4L2 device lifecycle internally — no caller-visible state is needed.
 * The struct exists solely to satisfy the HWContextType registration
 * (device_hwctx_size must be non-zero for av_hwdevice_ctx_alloc).
 */
typedef struct AVNVMPIDeviceContext {
    /**
     * Reserved for future use (e.g. device selection when multiple
     * Jetson decode/encode engines are available). Currently unused.
     */
    int reserved;
} AVNVMPIDeviceContext;

#endif /* AVUTIL_HWCONTEXT_NVMPI_H */
