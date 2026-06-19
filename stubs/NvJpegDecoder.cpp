/*
 * NvJpegDecoder stub — satisfies linker for off-Jetson CI builds.
 * All methods return error/NULL; no actual JPEG decoding occurs.
 *
 * Allocated by stubs build (WITH_STUBS); freed by normal teardown.
 */

#include "NvJpegDecoder.h"
#include <cstddef>
#include <cstring>

/* Constructor — stub, no hardware init. */
NvJPEGDecoder::NvJPEGDecoder(const char *comp_name)
    : NvElement(comp_name, valid_fields)
{
    memset(&cinfo, 0, sizeof(cinfo));
    memset(&jerr, 0, sizeof(jerr));
}

/* Destructor — stub, nothing to tear down. */
NvJPEGDecoder::~NvJPEGDecoder()
{
}

/* Factory — returns NULL (no HW available in stub build). */
NvJPEGDecoder *
NvJPEGDecoder::createJPEGDecoder(const char * /* comp_name */)
{
    return NULL;
}

/* decodeToFd — stub, always fails. */
int
NvJPEGDecoder::decodeToFd(int & /* fd */,
                           unsigned char * /* in_buf */,
                           unsigned long /* in_buf_size */,
                           uint32_t & /* pixfmt */,
                           uint32_t & /* width */,
                           uint32_t & /* height */)
{
    return -1;
}

/* decodeToBuffer — stub, always fails. */
int
NvJPEGDecoder::decodeToBuffer(NvBuffer ** /* buffer */,
                               unsigned char * /* in_buf */,
                               unsigned long /* in_buf_size */,
                               uint32_t * /* pixfmt */,
                               uint32_t * /* width */,
                               uint32_t * /* height */)
{
    return -1;
}

/* disableMjpegDecode — stub, no-op. */
void
NvJPEGDecoder::disableMjpegDecode()
{
}
