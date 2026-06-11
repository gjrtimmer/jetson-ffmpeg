/*
 * NVMPI_frameBuf.hpp — wrapper for one hardware DMA frame buffer
 * (libnvmpi layer; implementation in src/NVMPI_frameBuf.cpp).
 *
 * The decoder allocates a pool of these (see nvmpictx::initFramePool in
 * src/nvmpi_dec.cpp) as the pitch-linear destination buffers of the
 * VIC transform that converts/scales the decoder's block-linear output.
 * Instances circulate through NVMPI_bufPool<NVMPI_frameBuf*> between the
 * capture thread (fills them) and the user thread (copies them out).
 *
 * Works with both NVIDIA buffer APIs via nvUtils2NvBuf.h:
 *   - legacy nvbuf_utils: only the dmabuf fd is needed.
 *   - NvUtils/NvBufSurface (JetPack 5+, WITH_NVUTILS): additionally keeps
 *     the NvBufSurface* resolved from the fd, since the surface API takes
 *     surfaces instead of fds.
 */
#pragma once
#include "nvUtils2NvBuf.h"

struct NVMPI_frameBuf
{
#ifdef WITH_NVUTILS
	//Surface view of dst_dma_fd (not separately allocated; owned by the
	//underlying buffer and invalidated when the fd is destroyed).
	NvBufSurface *dst_dma_surface = NULL;
#endif
	//dmabuf file descriptor of the allocated buffer; -1 when unallocated.
	int dst_dma_fd = -1;
	//Presentation timestamp in microseconds, copied from the V4L2 capture
	//buffer when the frame is filled; read back in nvmpi_decoder_get_frame.
	unsigned long long timestamp = 0;

	//allocate DMA buffer
	//Allocates one hw buffer described by input_params (size/format/layout).
	//Returns true on success; on failure nothing is leaked and the struct
	//stays unallocated. The struct owns the buffer until destroy().
	bool alloc(NvBufferCreateParams& input_params);
	//destroy DMA buffer
	//Frees the buffer (no-op if not allocated). Returns false only if the
	//underlying NvBufferDestroy call fails.
	bool destroy();
};
