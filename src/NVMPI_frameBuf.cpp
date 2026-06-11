/*
 * NVMPI_frameBuf.cpp — DMA frame buffer allocation/destruction
 * (libnvmpi layer; see NVMPI_frameBuf.hpp for the role of this struct).
 *
 * Encapsulates the only place where the decoder's destination DMA buffers
 * are created and freed, with one branch per NVIDIA buffer API:
 * NvUtils/NvBufSurface (WITH_NVUTILS, JetPack 5+) vs legacy nvbuf_utils.
 */
#include "NVMPI_frameBuf.hpp"
#include <iostream> //LOG. TODO: add some LOG() define

//Allocate one hardware DMA buffer according to input_params and take
//ownership of it. Returns true on success. On the NvUtils path the
//NvBufSurface view of the new fd is also resolved (needed later by
//NvBufSurfTransform and the CPU map/sync helpers); if that fails the fd is
//destroyed again so no partially-initialized state remains.
bool NVMPI_frameBuf::alloc(NvBufferCreateParams& input_params)
{
	int ret = 0;
#ifdef WITH_NVUTILS
	//NvUtils path: allocate a single surface-array buffer; the fd is the
	//handle used for queueing/destruction.
	ret = NvBufSurf::NvAllocate(&input_params, 1, &dst_dma_fd);
	if(ret<0)
	{
		std::cerr << "Failed to allocate buffer" << std::endl;
		return false;
	}

	//Look up the NvBufSurface* behind the fd. This is a view, not a second
	//allocation — destroying the fd invalidates it.
	ret = NvBufSurfaceFromFd(dst_dma_fd, (void**)(&dst_dma_surface));
	if(ret<0)
	{
		std::cerr << "Failed to get surface for buffer" << std::endl;
		NvBufferDestroy(dst_dma_fd);
		dst_dma_fd = -1;
		return false;
	}
#else
	//Legacy nvbuf_utils path: a single call both allocates the buffer and
	//returns its dmabuf fd.
	ret = NvBufferCreateEx(&dst_dma_fd, &input_params);
	if(ret<0)
	{
		std::cerr << "Failed to allocate buffer" << std::endl;
		return false;
	}
#endif
	
	return true;
}

//Release the DMA buffer if one is held. Safe to call repeatedly / on an
//unallocated instance (then it is a no-op returning true). On the NvUtils
//build the cached surface pointer is cleared as well, since it became
//dangling when the fd was destroyed. NvBufferDestroy maps to
//NvBufSurf::NvDestroy under WITH_NVUTILS (see nvUtils2NvBuf.h).
bool NVMPI_frameBuf::destroy()
{
	int ret = 0;
	if(dst_dma_fd >= 0)
	{
		ret = NvBufferDestroy(dst_dma_fd);
		if(ret<0)
		{
			std::cerr << "Failed to Destroy NvBuffer" << std::endl;
			return false;
		}
		dst_dma_fd = -1;
#ifdef WITH_NVUTILS
		dst_dma_surface = NULL;
#endif
	}
	
	return true;
}
