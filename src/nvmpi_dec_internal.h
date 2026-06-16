#pragma once

#include "nvmpi.h"
#include "NvVideoDecoder.h"
#include "nvUtils2NvBuf.h"
#include "NVMPI_bufPool.hpp"
#include "nvmpi_frame_buffer.hpp"
#include <vector>
#include <iostream>
#include <thread>
#include <unistd.h>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>

//default max size (bytes) of one compressed input chunk on the OUTPUT
//plane; 4 MB proved too small for 4K high-bitrate I-frames. Overridable
//per-context via nvDecParam.chunk_size.
#define CHUNK_SIZE_DEFAULT 10000000
//upper bound for CAPTURE-plane DMA buffers (sizes the fd/surface arrays)
#define MAX_BUFFERS 32

//Error reporting helper: logs to stderr but does NOT abort or return —
//decoding continues on a best-effort basis (errorCode is unused).
#define TEST_ERROR(condition, message, errorCode)    \
	if (condition)                               \
{                                                    \
	std::cerr<< message;			     \
}

using namespace std;

//Decoder context behind the opaque nvmpictx* handle of the public API.
//Created by nvmpi_create_decoder(), owned by the caller via
//nvmpi_decoder_close(). Shared between the user thread and the internal
//capture thread; cross-thread handoff happens through framePool, and the
//'eos' flag is the (best-effort) shutdown signal.
struct nvmpictx
{
	NvVideoDecoder *dec{nullptr};       //NVIDIA V4L2 decoder device wrapper
	std::atomic<bool> eos{false};       //set on EOS or fatal error; stops capture loop
	                                    //(atomic: read by user thread, written by capture thread)
	int index{0};                       //next OUTPUT-plane buffer index until all are used once
	unsigned int coded_width{0};        //stream resolution reported by the decoder (crop)
	unsigned int coded_height{0};
	unsigned int output_width{0};       //resolution delivered to the user (resized or coded)
	unsigned int output_height{0};
	nvSize resized{0, 0};               //requested hw downscale target; {0,0} = no resize

	int numberCaptureBuffers{0};        //actual CAPTURE-plane DMA buffer count (min + 5)

	//dmabuf fds of the CAPTURE-plane buffers the decoder writes into
	int dmaBufferFileDescriptor[MAX_BUFFERS];

#ifdef WITH_NVUTILS
	//NvBufSurface views of the fds above (needed by NvBufSurfTransform)
	NvBufSurface *dmaBufferSurface[MAX_BUFFERS];
	//VIC transform session config (compute device selection etc.)
	NvBufSurfTransformConfigParams session;
#else
	NvBufferSession session;
#endif
	//cached parameters for the per-frame VIC transform (crop rects, filter)
	NvBufferTransformParams transform_params;
	NvBufferRect src_rect, dest_rect;

	nvPixFormat out_pixfmt;             //user-requested output layout (NV12/YUV420)
	unsigned int decoder_pixfmt{0};     //V4L2 fourcc of the compressed input
	std::thread dec_capture_loop;       //runs dec_capture_loop_fcn()

	int frame_pool_size{12};            //number of nvmpi_frame_buffer's to allocate
	uint32_t chunk_size{CHUNK_SIZE_DEFAULT}; //bytes per compressed-input OUTPUT-plane buffer
	bool max_perf{true};                //lift NVDEC clock governor for max throughput
	bool disable_dpb{false};            //skip DPB reordering (low-latency, B-frame-free only)
	int wait_timeout_ms{500};           //blocking dequeue ceiling (ms); set via AVOption
	//producer/consumer pool: capture thread fills, user thread consumes
	NVMPI_bufPool<nvmpi_frame_buffer*>* framePool;
	//all frame bufs ever allocated by initFramePool — ensures deinitFramePool
	//can destroy every buffer even if one is temporarily outside both queues
	std::vector<nvmpi_frame_buffer*> allocatedFrameBufs;

	//output frame size params
	//(describes the pitch-linear dst_dma buffers; filled by
	// updateFrameSizeParams() and used when copying out to the user)
	unsigned int num_planes;
	unsigned int frame_linesize[MAX_NUM_PLANES]; //stride (pitch) of each plane in bytes
	unsigned int frame_height[MAX_NUM_PLANES];   //number of lines in each plane
	unsigned int frame_linedatasize[MAX_NUM_PLANES]; //usable data size for 1 line

	//empty frame queue and free buffers memory
	void deinitFramePool();
	//alloc frame buffers based on frame_size data in nvmpictx
	void initFramePool();

	//get dst_dma buffer params and set corresponding frame size and linesize in nvmpictx
	void updateFrameSizeParams();
	//refresh src/dst rects and filter settings for the per-frame transform
	void updateBufferTransformParams();

	//allocate CAPTURE-plane DMA buffers, REQBUFS/STREAMON, enqueue them all
	void initDecoderCapturePlane(v4l2_format &format);
	/* deinitPlane unmaps the buffers and calls REQBUFS with count 0 */
	void deinitDecoderCapturePlane();
};

//Forward declarations
NvBufferColorFormat getNvColorFormatFromV4l2Format(v4l2_format &format, bool want_10bit);
void dec_capture_loop_fcn(void *arg);
void respondToResolutionEvent(v4l2_format &format, v4l2_crop &crop, nvmpictx* ctx);
int copyNvBufToFrame(nvmpictx* ctx, nvmpi_frame_buffer *nvmpiBuf, nvFrame* frame);
