/*
 * nvmpi_dec_drain.h -- end-of-stream handling for the V4L2 video decoders
 * (h264/hevc/vp8/vp9/mpeg2/mpeg4/av1_nvmpi).
 *
 * A zero-size packet is libnvmpi's EOS marker. Without it the hardware
 * decoder never flushes the frames it holds for reordering (the DPB depth),
 * so the last frames of a stream were lost (jetson-ffmpeg#149). libnvmpi
 * releases before the jetson-nvmpi#161 fix stop their capture thread on the
 * marker and lose the tail anyway, so the marker is only sent to a libnvmpi
 * that advertises NVMPI_CAP_DEC_EOS_DRAIN through nvmpi_get_caps()
 * (libnvmpi >= 4.7.0).
 *
 * Included by nvmpi_dec.c; static inline, not a separate translation unit.
 */
#ifndef NVMPI_DEC_DRAIN_H
#define NVMPI_DEC_DRAIN_H

#include <stdlib.h>

/* True when libnvmpi drains after a zero-size EOS packet. The gate is the
 * capability bit, not a version: libnvmpi older than 4.7.0 does not export
 * nvmpi_get_caps (symbol NULL, optional load), which reads as "no drain
 * support", the safe legacy behaviour (no EOS marker, bounded blocking get).
 * NVMPI_FORCE_NO_DEC_EOS_DRAIN=1 (test hook) fakes such a library. The
 * answer cannot change while the library stays loaded, so it is cached. */
static inline int nvmpi_has_dec_eos_drain(void)
{
    static int cached = -1;
    const char *force = getenv("NVMPI_FORCE_NO_DEC_EOS_DRAIN");

    if (force && force[0] == '1')
        return 0;
    if (cached < 0)
        cached = nvmpi_get_caps_dl &&
                 (nvmpi_get_caps_dl() & NVMPI_CAP_DEC_EOS_DRAIN) ? 1 : 0;
    return cached;
}

#endif /* NVMPI_DEC_DRAIN_H */
