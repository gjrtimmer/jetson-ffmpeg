/*
 * Tegra-specific libjpeg extension stubs — satisfies linker for off-Jetson
 * CI builds. On real Jetson hardware these symbols are provided by NVIDIA's
 * custom libjpeg.so.8 (libjpeg-8b with hardware acceleration extensions).
 *
 * Symbols stubbed:
 *   jpeg_set_hardware_acceleration_parameters_enc  (used by NvJpegEncoder)
 *   jpeg_set_hardware_acceleration_parameters_dec  (used by NvJpegDecoder)
 *
 * Allocated by stubs build (WITH_STUBS); no-op at runtime.
 */

#include <stdio.h>
#include "jpeglib.h"

/* Encoder HW accel — stub, no-op. */
void
jpeg_set_hardware_acceleration_parameters_enc(j_compress_ptr cinfo,
                                               boolean hw_acceleration,
                                               unsigned int defaultBuffSize,
                                               unsigned int defaultWidth,
                                               unsigned int defaultHeight)
{
    (void)cinfo;
    (void)hw_acceleration;
    (void)defaultBuffSize;
    (void)defaultWidth;
    (void)defaultHeight;
}

/* Decoder HW accel — stub, no-op. */
void
jpeg_set_hardware_acceleration_parameters_dec(j_decompress_ptr cinfo,
                                               boolean hw_acceleration,
                                               unsigned int defaultBuffSize,
                                               unsigned int defaultWidth,
                                               unsigned int defaultHeight,
                                               unsigned int defaultBitstreamBuffSize,
                                               boolean rgb565)
{
    (void)cinfo;
    (void)hw_acceleration;
    (void)defaultBuffSize;
    (void)defaultWidth;
    (void)defaultHeight;
    (void)defaultBitstreamBuffSize;
    (void)rgb565;
}
