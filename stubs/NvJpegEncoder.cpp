/*
 * NvJpegEncoder stub — satisfies linker for off-Jetson CI builds.
 * All methods return error/NULL; no actual JPEG encoding occurs.
 *
 * Allocated by stubs build (WITH_STUBS); freed by normal teardown.
 */

#include "NvJpegEncoder.h"
#include <cstddef>
#include <cstring>

/* Constructor — stub, no hardware init. */
NvJPEGEncoder::NvJPEGEncoder(const char *comp_name)
    : NvElement(comp_name, valid_fields)
{
    memset(&cinfo, 0, sizeof(cinfo));
    memset(&jerr, 0, sizeof(jerr));
}

/* Destructor — stub, nothing to tear down. */
NvJPEGEncoder::~NvJPEGEncoder()
{
}

/* Factory — returns NULL (no HW available in stub build). */
NvJPEGEncoder *
NvJPEGEncoder::createJPEGEncoder(const char * /* comp_name */)
{
    return NULL;
}

/* encodeFromFd — stub, always fails. */
int
NvJPEGEncoder::encodeFromFd(int /* fd */,
                             J_COLOR_SPACE /* color_space */,
                             unsigned char ** /* out_buf */,
                             unsigned long & /* out_buf_size */,
                             int /* quality */)
{
    return -1;
}

/* encodeFromBuffer — stub, always fails. */
int
NvJPEGEncoder::encodeFromBuffer(NvBuffer & /* buffer */,
                                 J_COLOR_SPACE /* color_space */,
                                 unsigned char ** /* out_buf */,
                                 unsigned long & /* out_buf_size */,
                                 int /* quality */)
{
    return -1;
}

/* setCropRect — stub, no-op. */
void
NvJPEGEncoder::setCropRect(uint32_t /* left */,
                            uint32_t /* top */,
                            uint32_t /* width */,
                            uint32_t /* height */)
{
}

/* setScaledEncodeParams — stub, no-op. */
void
NvJPEGEncoder::setScaledEncodeParams(uint32_t /* scale_width */,
                                      uint32_t /* scale_height */)
{
}
