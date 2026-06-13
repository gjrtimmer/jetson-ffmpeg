/*
 * nvmpi_dec.cpp — hardware video decoder pipeline of libnvmpi (layer 1).
 *
 * Implements the decoder half of the public C API in include/nvmpi.h on top
 * of NVIDIA's V4L2 NvVideoDecoder sample class (from the Jetson Multimedia
 * API). The FFmpeg wrapper (ffmpeg/dev/common/libavcodec/nvmpi_dec.c) is
 * the primary consumer.
 *
 * V4L2 decoder model (M2M device, two queues):
 *   - OUTPUT plane  = input side: compressed bitstream chunks go in.
 *   - CAPTURE plane = output side: decoded (block-linear NV12) frames
 *     come out into DMA buffers we allocate and enqueue.
 *
 * Data flow / threading:
 *   user thread:    nvmpi_decoder_put_packet() -> memcpy packet into an
 *                   OUTPUT-plane buffer -> qBuffer (blocks on dqBuffer once
 *                   all buffers are in flight).
 *   capture thread: dec_capture_loop_fcn() waits for the resolution-change
 *                   event, (re)allocates CAPTURE-plane DMA buffers, then
 *                   loops: dqBuffer decoded frame -> hw transform (VIC) into
 *                   a pitch-linear NVMPI_frameBuf taken from the frame
 *                   pool's "empty" queue (converting to NV12/YUV420 and
 *                   optionally scaling) -> push to the pool's "filled"
 *                   queue -> re-queue the V4L2 buffer.
 *   user thread:    nvmpi_decoder_get_frame() -> pop a filled NVMPI_frameBuf,
 *                   memcpy planes into the caller's nvFrame, return the
 *                   buffer to the "empty" queue.
 *
 * Both NVIDIA buffer APIs are supported: NvUtils/NvBufSurface when
 * WITH_NVUTILS is defined (JetPack 5+), legacy nvbuf_utils otherwise
 * (see include/nvUtils2NvBuf.h).
 */
#include "nvmpi.h"
#include "NvVideoDecoder.h"
#include "nvUtils2NvBuf.h"
#include "NVMPI_bufPool.hpp"
#include "NVMPI_frameBuf.hpp"
#include <vector>
#include <iostream>
#include <thread>
#include <unistd.h>
#include <queue>
#include <mutex>
#include <condition_variable>

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
	bool eos{false};                    //set on EOS or fatal error; stops capture loop
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

	int frame_pool_size{12};            //number of NVMPI_frameBuf's to allocate
	uint32_t chunk_size{CHUNK_SIZE_DEFAULT}; //bytes per compressed-input OUTPUT-plane buffer
	//producer/consumer pool: capture thread fills, user thread consumes
	NVMPI_bufPool<NVMPI_frameBuf*>* framePool;
	//all frame bufs ever allocated by initFramePool — ensures deinitFramePool
	//can destroy every buffer even if one is temporarily outside both queues
	std::vector<NVMPI_frameBuf*> allocatedFrameBufs;

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

//Map the colorspace/quantization reported by the decoder on its CAPTURE
//plane to the matching NvBuffer NV12 color format variant (BT.601/709/2020,
//standard vs extended luma range). Used when allocating the CAPTURE-plane
//DMA buffers so the subsequent VIC transform interprets colors correctly.
NvBufferColorFormat getNvColorFormatFromV4l2Format(v4l2_format &format)
{
	NvBufferColorFormat ret_cf = NvBufferColorFormat_NV12; 
	switch (format.fmt.pix_mp.colorspace)
	{
		case V4L2_COLORSPACE_SMPTE170M:
			if (format.fmt.pix_mp.quantization == V4L2_QUANTIZATION_DEFAULT)
			{
				// "Decoder colorspace ITU-R BT.601 with standard range luma (16-235)"
				ret_cf = NvBufferColorFormat_NV12;
			}
			else
			{
				//"Decoder colorspace ITU-R BT.601 with extended range luma (0-255)";
				ret_cf = NvBufferColorFormat_NV12_ER;
			}
			break;
		case V4L2_COLORSPACE_REC709:
			if (format.fmt.pix_mp.quantization == V4L2_QUANTIZATION_DEFAULT)
			{
				//"Decoder colorspace ITU-R BT.709 with standard range luma (16-235)";
				ret_cf = NvBufferColorFormat_NV12_709;
			}
			else
			{
				//"Decoder colorspace ITU-R BT.709 with extended range luma (0-255)";
				ret_cf = NvBufferColorFormat_NV12_709_ER;
			}
			break;
		case V4L2_COLORSPACE_BT2020:
			{
				//"Decoder colorspace ITU-R BT.2020";
				ret_cf = NvBufferColorFormat_NV12_2020;
			}
			break;
		default:
			if (format.fmt.pix_mp.quantization == V4L2_QUANTIZATION_DEFAULT)
			{
				//"Decoder colorspace ITU-R BT.601 with standard range luma (16-235)";
				ret_cf = NvBufferColorFormat_NV12;
			}
			else
			{
				//"Decoder colorspace ITU-R BT.601 with extended range luma (0-255)";
				ret_cf = NvBufferColorFormat_NV12_ER;
			}
			break;
	}
	return ret_cf;
}


//(Re)initialize the decoder CAPTURE plane after a resolution-change event:
//set the negotiated plane format, allocate numberCaptureBuffers block-linear
//NV12 DMA buffers sized to the coded resolution, REQBUFS them as
//V4L2_MEMORY_DMABUF, start streaming and enqueue every buffer so the
//decoder can start writing. Called from the capture thread only.
void nvmpictx::initDecoderCapturePlane(v4l2_format &format)
{
	int ret=0;
	int32_t minimumDecoderCaptureBuffers;
	NvBufferCreateParams cParams;
	memset(&cParams, 0, sizeof(cParams));
	
	ret=dec->setCapturePlaneFormat(format.fmt.pix_mp.pixelformat,format.fmt.pix_mp.width,format.fmt.pix_mp.height);
	TEST_ERROR(ret < 0, "Error in setting decoder capture plane format", ret);

	dec->getMinimumCapturePlaneBuffers(minimumDecoderCaptureBuffers);
	TEST_ERROR(ret < 0, "Error while getting value of minimum capture plane buffers",ret);

	/* Request (min + extra) buffers, export and map buffers. */
	numberCaptureBuffers = minimumDecoderCaptureBuffers + 5;

	//Block-linear layout matches what the hw decoder writes natively; the
	//buffers are converted to pitch-linear later by the VIC transform.
	cParams.colorFormat = getNvColorFormatFromV4l2Format(format);
	cParams.width = coded_width;
	cParams.height = coded_height;
	cParams.layout = NvBufferLayout_BlockLinear;
#ifdef WITH_NVUTILS
	cParams.memType = NVBUF_MEM_SURFACE_ARRAY;
	cParams.memtag = NvBufSurfaceTag_VIDEO_DEC;

	//NvUtils path: allocate all buffers in one call, then resolve the
	//NvBufSurface view of each fd for use with NvBufSurfTransform.
	ret = NvBufSurf::NvAllocate(&cParams, numberCaptureBuffers, dmaBufferFileDescriptor);
	TEST_ERROR(ret < 0, "Failed to create buffers", error);
	for (int index = 0; index < numberCaptureBuffers; index++)
	{
		ret = NvBufSurfaceFromFd(dmaBufferFileDescriptor[index], (void**)(&(dmaBufferSurface[index])));
		TEST_ERROR(ret < 0, "Failed to get surface for buffer", ret);
	}
#else
	//legacy nvbuf_utils path: allocate the buffers one by one
	cParams.payloadType = NvBufferPayload_SurfArray;
	cParams.nvbuf_tag = NvBufferTag_VIDEO_DEC;

	for (int index = 0; index < numberCaptureBuffers; index++)
	{
		ret = NvBufferCreateEx(&dmaBufferFileDescriptor[index], &cParams);
		TEST_ERROR(ret < 0, "Failed to create buffers", ret);
	}
#endif

    /* Request buffers on decoder capture plane. Refer ioctl VIDIOC_REQBUFS */
	dec->capture_plane.reqbufs(V4L2_MEMORY_DMABUF, numberCaptureBuffers);
	TEST_ERROR(ret < 0, "Error in decoder capture plane streamon", ret);

    /* Decoder capture plane STREAMON. Refer ioctl VIDIOC_STREAMON */
	dec->capture_plane.setStreamStatus(true);
	TEST_ERROR(ret < 0, "Error in decoder capture plane streamon", ret);

	/* Enqueue all the empty decoder capture plane buffers. */
	for (uint32_t i = 0; i < dec->capture_plane.getNumBuffers(); i++)
	{
		struct v4l2_buffer v4l2_buf;
		struct v4l2_plane planes[MAX_PLANES];

		memset(&v4l2_buf, 0, sizeof(v4l2_buf));
		memset(planes, 0, sizeof(planes));

		v4l2_buf.index = i;
		v4l2_buf.m.planes = planes;
		v4l2_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		v4l2_buf.memory = V4L2_MEMORY_DMABUF;
		v4l2_buf.m.planes[0].m.fd = dmaBufferFileDescriptor[i];

		ret = dec->capture_plane.qBuffer(v4l2_buf, NULL);
		TEST_ERROR(ret < 0, "Error Qing buffer at output plane", ret);
	}
	
	return;
}

//Tear down the CAPTURE plane: STREAMOFF, unmap/release the V4L2 buffers
//(deinitPlane issues REQBUFS with count 0) and destroy our DMA buffers.
//Called on resolution change (before re-init) and from nvmpi_decoder_close.
void nvmpictx::deinitDecoderCapturePlane()
{
	if (numberCaptureBuffers == 0)
		return;

	int ret = 0;
	dec->capture_plane.setStreamStatus(false);
	// bypass deinitPlane() — it calls waitForDQThread() which touches MMAPI
	// DQ thread state the decoder never uses (we use std::thread instead);
	// for DMABUF the only work deinitPlane() does beyond that is reqbufs(0)
	dec->capture_plane.reqbufs(V4L2_MEMORY_DMABUF, 0);
	for (int index = 0; index < numberCaptureBuffers; index++)
	{
		if (dmaBufferFileDescriptor[index] >= 0)
		{
			ret = NvBufferDestroy(dmaBufferFileDescriptor[index]);
			TEST_ERROR(ret < 0, "Failed to Destroy NvBuffer", ret);
			dmaBufferFileDescriptor[index] = -1;
		}
	}
	numberCaptureBuffers = 0;
	return;
}

//Query the actual plane geometry (count, pitch, height, bytes-per-line) of
//one freshly allocated dst_dma frame buffer and cache it in the context.
//These values drive the memcpy in copyNvBufToFrame(); the allocator may
//choose pitches different from width, so they must be read back, not
//assumed. Uses peekEmptyBuf() — safe only because no consumer runs yet.
void nvmpictx::updateFrameSizeParams()
{
	//it's safe when called from respondToResolutionEvent() after initFramePool()
	NVMPI_frameBuf* fb = framePool->peekEmptyBuf();
#ifdef WITH_NVUTILS
	NvBufSurfacePlaneParams parm;
	NvBufSurfaceParams dst_dma_surface_params;
	dst_dma_surface_params = fb->dst_dma_surface->surfaceList[0];
	parm = dst_dma_surface_params.planeParams;
#else
	NvBufferParams parm;
	int ret = NvBufferGetParams(fb->dst_dma_fd, &parm);
	TEST_ERROR(ret < 0, "Failed to get dst dma buf params", ret);
#endif

	num_planes = parm.num_planes;
	for(unsigned int i=0; i<num_planes; i++)
	{
		frame_linesize[i] = parm.pitch[i];
		frame_height[i] = parm.height[i];
#ifdef WITH_NVUTILS
		frame_linedatasize[i] = parm.width[i] * parm.bytesPerPix[i]; //valid only for nvutils
#else
		//legacy API lacks bytesPerPix: the interleaved UV plane of NV12
		//variants carries 2 bytes per pixel, everything else 1.
		if(i == 1 && (parm.pixel_format == NvBufferColorFormat_NV12 ||
				parm.pixel_format == NvBufferColorFormat_NV16 ||
				parm.pixel_format == NvBufferColorFormat_NV24 ||
				parm.pixel_format == NvBufferColorFormat_NV12_ER ||
				parm.pixel_format == NvBufferColorFormat_NV12_709 ||
				parm.pixel_format == NvBufferColorFormat_NV12_709_ER ||
				parm.pixel_format == NvBufferColorFormat_NV12_2020))
		{
			frame_linedatasize[i] = parm.width[i] * 2;
		}
		else
		{
			frame_linedatasize[i] = parm.width[i];
		}
#endif
	}
	
	return;
}

//Build the cached VIC transform parameters used for every decoded frame:
//src rect = coded (cropped) decoder resolution, dst rect = user-visible
//output resolution (downscale happens here when the resize option is set).
//The two APIs differ slightly: NvUtils takes rect pointers, the legacy API
//embeds the rects and needs the explicit session handle.
void nvmpictx::updateBufferTransformParams()
{
	src_rect.top = 0;
	src_rect.left = 0;
	src_rect.width = coded_width;
	src_rect.height = coded_height;
	dest_rect.top = 0;
	dest_rect.left = 0;
	dest_rect.width = output_width;
	dest_rect.height = output_height;
	
	memset(&transform_params,0,sizeof(transform_params));
	transform_params.transform_flag = NVBUFFER_TRANSFORM_FILTER;
	transform_params.transform_flip = NvBufferTransform_None;
	transform_params.transform_filter = NvBufferTransform_Filter_Smart;
	//ctx->transform_params.transform_filter = NvBufSurfTransformInter_Nearest;
#ifdef WITH_NVUTILS
	transform_params.src_rect = &src_rect;
	transform_params.dst_rect = &dest_rect;
#else
	transform_params.src_rect = src_rect;
	transform_params.dst_rect = dest_rect;
	transform_params.session = session;
#endif
}

//Destroy all frame buffers tracked by allocatedFrameBufs and drain both
//pool queues. Uses the vector instead of the queues so that buffers
//temporarily outside both queues (e.g. held by a concurrent get_frame
//call during resolution change) are still freed.
void nvmpictx::deinitFramePool()
{
	while(framePool->dqEmptyBuf()) {}
	while(framePool->dqFilledBuf()) {}

	for (auto* fb : allocatedFrameBufs)
	{
		fb->destroy();
		delete fb;
	}
	allocatedFrameBufs.clear();
}

//Allocate frame_pool_size pitch-linear DMA buffers at the output resolution
//and user-requested pixel format, and seed the pool's "empty" queue with
//them. These are the destination buffers of the VIC transform — distinct
//from the block-linear CAPTURE-plane buffers the decoder writes into.
void nvmpictx::initFramePool()
{
	//if(bufNumber <= 0) return false; //TODO log msg //TODO check if it's already allocated and deinit first
	NvBufferColorFormat cFmt = out_pixfmt==NV_PIX_NV12?NvBufferColorFormat_NV12: NvBufferColorFormat_YUV420;
	
	NvBufferCreateParams input_params;
	memset(&input_params, 0, sizeof(input_params));
	/* Create PitchLinear output buffer for transform. */
	input_params.width = output_width;
	input_params.height = output_height;
	input_params.layout = NvBufferLayout_Pitch;
	input_params.colorFormat = cFmt;
#ifdef WITH_NVUTILS
	input_params.memType = NVBUF_MEM_SURFACE_ARRAY;
	input_params.memtag = NvBufSurfaceTag_VIDEO_CONVERT;
#else
	input_params.payloadType = NvBufferPayload_SurfArray;
	input_params.nvbuf_tag = NvBufferTag_VIDEO_DEC;
#endif
	
	for(int i=0;i<frame_pool_size;i++)
	{
		NVMPI_frameBuf* fb = new NVMPI_frameBuf();
		if(!fb->alloc(input_params)) { delete fb; break; }
		allocatedFrameBufs.push_back(fb);
		framePool->qEmptyBuf(fb);
	}
	return;
}

//Handle V4L2_EVENT_RESOLUTION_CHANGE (also fired once at stream start when
//the decoder has parsed the headers): query the new format/crop, rebuild
//the CAPTURE plane and the destination frame pool, and refresh the cached
//transform/copy parameters. Runs on the capture thread.
void respondToResolutionEvent(v4l2_format &format, v4l2_crop &crop,nvmpictx* ctx)
{
	int ret=0;

    /* Get capture plane format from the decoder.
       This may change after resolution change event.
       Refer ioctl VIDIOC_G_FMT */
	ret = ctx->dec->capture_plane.getFormat(format);	
	TEST_ERROR(ret < 0, "Error: Could not get format from decoder capture plane", ret);

    /* Get the display resolution from the decoder.
       Refer ioctl VIDIOC_G_CROP */
	ret = ctx->dec->capture_plane.getCrop(crop);
	TEST_ERROR(ret < 0, "Error: Could not get crop from decoder capture plane", ret);

	//output resolution = resize target if requested, else stream resolution
	ctx->coded_width = crop.c.width;
	ctx->coded_height = crop.c.height;
	ctx->output_width = ctx->resized.width ? ctx->resized.width : crop.c.width;
	ctx->output_height = ctx->resized.height ? ctx->resized.height : crop.c.height;

	//init/reinit DecoderCapturePlane
#ifndef WITH_NVUTILS
	bool hadCapture = ctx->numberCaptureBuffers > 0;
#endif
	ctx->deinitDecoderCapturePlane();
	ctx->initDecoderCapturePlane(format);
	
	/* override default seesion. Without overriding session we wil
	   get seg. fault if decoding in forked process*/
#ifdef WITH_NVUTILS
	ctx->session.compute_mode = NvBufSurfTransformCompute_VIC;
	ctx->session.gpu_id = 0;
	ctx->session.cuda_stream = 0;
	NvBufSurfTransformSetSessionParams(&(ctx->session));
#else
	if (hadCapture)
		NvBufferSessionDestroy(ctx->session);
	ctx->session = NvBufferSessionCreate();
#endif

	ctx->deinitFramePool();
	ctx->initFramePool();
	//get dst_dma buffer params and set corresponding frame size and linesize in nvmpictx
	ctx->updateFrameSizeParams();
	
	//reset buffer transformation params based on new resolution data
	ctx->updateBufferTransformParams();
	
	return;
}

/*
struct transFormWorker
{
	std::thread _workerThr;
	nvmpictx* _ctx = NULL;
	
	//void init(nvmpictx* ctx);
	
	void start(nvmpictx* ctx);
	void stop();
	
	bool isWaiting();
	
	void workerFnc();
};

void transFormWorker::start(nvmpictx* ctx)
{
	_ctx = ctx;
	_workerThr = std::thread(&transFormWorker::workerFnc,this);
	return;
}

void transFormWorker::workerFnc()
{
	
}
*/

//Body of the decoder capture thread (started by nvmpi_create_decoder).
//Responsibilities:
//  1. block until the first resolution-change event so buffer sizes are
//     known, then set up the CAPTURE plane and frame pool;
//  2. loop: dequeue decoded CAPTURE buffers, VIC-transform each into an
//     "empty" NVMPI_frameBuf from the pool (format convert + optional
//     scale), stamp the pts, publish it as "filled", and re-queue the V4L2
//     buffer to the decoder;
//  3. handle mid-stream resolution changes and EOS/error shutdown.
//Exits when ctx->eos is set (EOS buffer, fatal error, or close()).
void dec_capture_loop_fcn(void *arg)
{
	nvmpictx* ctx=(nvmpictx*)arg;
	NvVideoDecoder *dec = ctx->dec;
	
	struct v4l2_format v4l2Format;
	struct v4l2_crop v4l2Crop;
	struct v4l2_event v4l2Event;
	int ret;
	NVMPI_frameBuf* fb = NULL;
	//std::thread transformWorkersPool[3];

    /* Need to wait for the first Resolution change event, so that
       the decoder knows the stream resolution and can allocate appropriate
       buffers when we call REQBUFS. */
    do
    {
        /* Refer ioctl VIDIOC_DQEVENT */
        ret = dec->dqEvent(v4l2Event, 500);
        if (ret < 0)
        {
            if (errno == EAGAIN)
            {
                continue;
            }
            else
            {
               ERROR_MSG("Error in dequeueing decoder event");
               ctx->eos=true;
            }
        }
    }
    while ((v4l2Event.type != V4L2_EVENT_RESOLUTION_CHANGE) && !ctx->eos);

    /* Received the resolution change event, now can do respondToResolutionEvent. */
    if (!ctx->eos) respondToResolutionEvent(v4l2Format, v4l2Crop, ctx);
	
	while (!(ctx->eos || dec->isInError()))
	{
		NvBuffer *dec_buffer;
		
		// Check for Resolution change again.
		ret = dec->dqEvent(v4l2Event, false);
		if (ret == 0)
		{
			switch (v4l2Event.type)
			{
				case V4L2_EVENT_RESOLUTION_CHANGE:
					respondToResolutionEvent(v4l2Format, v4l2Crop, ctx);
					continue;
			}
		}
		
		/* Decoder capture loop */
		while(!ctx->eos)
		{
			struct v4l2_buffer v4l2_buf;
			struct v4l2_plane planes[MAX_PLANES];
			v4l2_buf.m.planes = planes;
			
			/* Dequeue a filled buffer. */
			//(0 retries: EAGAIN means nothing decoded yet, poll again;
			// the V4L2_BUF_FLAG_LAST flag marks the final buffer => EOS)
			if (dec->capture_plane.dqBuffer(v4l2_buf, &dec_buffer, NULL, 0))
			{
				if (errno == EAGAIN)
				{
					if (v4l2_buf.flags & V4L2_BUF_FLAG_LAST)
					{
						ERROR_MSG("Got EoS at capture plane");
						ctx->eos=true;
					}
					usleep(1000);
				}
				else
				{
					ERROR_MSG("Error while calling dequeue at capture plane");
					ctx->eos=true;
				}
				break;
			}
			
			//point the buffer at the DMA fd it was queued with
			dec_buffer->planes[0].fd = ctx->dmaBufferFileDescriptor[v4l2_buf.index];

			//grab a free destination buffer from the pool (non-blocking)
			fb = ctx->framePool->dqEmptyBuf();

			if(fb)
			{
				//hw transform: block-linear decoder output -> pitch-linear
				//dst buffer, converting format and scaling as configured
#ifdef WITH_NVUTILS
				ret = NvBufSurfTransform(ctx->dmaBufferSurface[v4l2_buf.index], fb->dst_dma_surface, &(ctx->transform_params));
#else
				ret = NvBufferTransform(dec_buffer->planes[0].fd, fb->dst_dma_fd, &(ctx->transform_params));
#endif
				TEST_ERROR(ret==-1, "Transform failed",ret);
				//carry the pts through (V4L2 timeval -> microseconds)
				fb->timestamp = (v4l2_buf.timestamp.tv_usec % 1000000) + (v4l2_buf.timestamp.tv_sec * 1000000UL);

				//hand the filled frame to the consumer (user thread)
				ctx->framePool->qFilledBuf(fb);
			}
			else
			{
				//no buffers available in the pool. wait for EOS or for user to read.
				//Backpressure: poll every 500us until the user returns a
				//buffer via nvmpi_decoder_get_frame(), then do the same
				//transform/publish as above. The V4L2 buffer is held back
				//meanwhile, eventually stalling the decoder.
				while(!ctx->eos)
				{
					std::this_thread::sleep_for(std::chrono::microseconds(500));
					fb = ctx->framePool->dqEmptyBuf();
					if(fb)
					{
#ifdef WITH_NVUTILS
						ret = NvBufSurfTransform(ctx->dmaBufferSurface[v4l2_buf.index], fb->dst_dma_surface, &(ctx->transform_params));
#else
						ret = NvBufferTransform(dec_buffer->planes[0].fd, fb->dst_dma_fd, &(ctx->transform_params));
#endif
						TEST_ERROR(ret==-1, "Transform failed",ret);
						fb->timestamp = (v4l2_buf.timestamp.tv_usec % 1000000) + (v4l2_buf.timestamp.tv_sec * 1000000UL);
						
						ctx->framePool->qFilledBuf(fb);
						break;
					}
				}
			}

			//return the DMA buffer to the decoder so it can be refilled
			v4l2_buf.m.planes[0].m.fd = ctx->dmaBufferFileDescriptor[v4l2_buf.index];
			if (dec->capture_plane.qBuffer(v4l2_buf, NULL) < 0)
			{
				ERROR_MSG("Error while queueing buffer at decoder capture plane");
			}
		}
	}
	return;
}

//TODO: accept in nvmpi_create_decoder input stream params (width and height, etc...) from ffmpeg.
//Public API: create and start a decoder.
//Opens the V4L2 decoder device, subscribes to resolution-change events,
//configures the OUTPUT (bitstream) plane with USERPTR buffers and starts
//streaming on it, then spawns the capture thread. The CAPTURE plane and
//frame pool are NOT set up here — that happens on the capture thread once
//the first resolution-change event reveals the stream geometry.
//Returns the new context (errors are currently only logged, not returned).
nvmpictx* nvmpi_create_decoder(nvDecParam* param)
{
	int ret;
	log_level = LOG_LEVEL_INFO;

	nvmpictx* ctx=new nvmpictx();

	ctx->dec = NvVideoDecoder::createVideoDecoder("dec0");
	TEST_ERROR(!ctx->dec, "Could not create decoder",ret);

	ret=ctx->dec->subscribeEvent(V4L2_EVENT_RESOLUTION_CHANGE, 0, 0);
	TEST_ERROR(ret < 0, "Could not subscribe to V4L2_EVENT_RESOLUTION_CHANGE", ret);
	
	ctx->frame_pool_size = param->frame_pool_size;

	//0 keeps the default; out-of-range values (including garbage from
	//callers built against an older nvDecParam layout) fall back too.
	if(param->chunk_size >= 65536 && param->chunk_size <= 64u*1024*1024)
		ctx->chunk_size = param->chunk_size;
	else if(param->chunk_size != 0)
		std::cerr << "[libnvmpi][W]: chunk_size " << param->chunk_size
		          << " out of range [65536, 67108864]; using default " << ctx->chunk_size << std::endl;

	//map the API codec enum to the V4L2 compressed pixel format fourcc
	switch(param->codingType)
	{
		case NV_VIDEO_CodingH264:
			ctx->decoder_pixfmt=V4L2_PIX_FMT_H264;
			break;
		case NV_VIDEO_CodingHEVC:
			ctx->decoder_pixfmt=V4L2_PIX_FMT_H265;
			break;
		case NV_VIDEO_CodingMPEG4:
			ctx->decoder_pixfmt=V4L2_PIX_FMT_MPEG4;
			break;
		case NV_VIDEO_CodingMPEG2:
			ctx->decoder_pixfmt=V4L2_PIX_FMT_MPEG2;
			break;
		case NV_VIDEO_CodingVP8:
			ctx->decoder_pixfmt=V4L2_PIX_FMT_VP8;
			break;
		case NV_VIDEO_CodingVP9:
			ctx->decoder_pixfmt=V4L2_PIX_FMT_VP9;
			break;
		default:
			ctx->decoder_pixfmt=V4L2_PIX_FMT_H264;
			break;
	}

	//OUTPUT plane: compressed input, buffers sized ctx->chunk_size
	ret=ctx->dec->setOutputPlaneFormat(ctx->decoder_pixfmt, ctx->chunk_size);

	TEST_ERROR(ret < 0, "Could not set output plane format", ret);

	//input mode 0 = feed complete NAL units / frames per buffer
	ret = ctx->dec->setFrameInputMode(0);
	TEST_ERROR(ret < 0, "Error in decoder setFrameInputMode for NALU", ret);
	
	//TODO: create option to enable max performace mode (?)
	//ret = ctx->dec->setMaxPerfMode(true);
	//TEST_ERROR(ret < 0, "Error while setting decoder to max perf", ret);

	//10 USERPTR buffers on the OUTPUT plane; packet data is memcpy'd into
	//them in nvmpi_decoder_put_packet()
	ret = ctx->dec->output_plane.setupPlane(V4L2_MEMORY_USERPTR, 10, false, true);
	TEST_ERROR(ret < 0, "Error while setting up output plane", ret);

	ctx->dec->output_plane.setStreamStatus(true);
	TEST_ERROR(ret < 0, "Error in output plane stream on", ret);

	ctx->out_pixfmt=param->pixFormat;
	ctx->resized = param->resized;
	ctx->framePool = new NVMPI_bufPool<NVMPI_frameBuf*>();
	ctx->eos=false;
	ctx->index=0;
	for(int index=0;index<MAX_BUFFERS;index++)
		ctx->dmaBufferFileDescriptor[index]=-1;
	ctx->numberCaptureBuffers=0;
	ctx->dec_capture_loop = std::thread(dec_capture_loop_fcn,ctx);
	pthread_setname_np(ctx->dec_capture_loop.native_handle(), "dec_capture");

	return ctx;
}

//Public API: feed one compressed packet to the decoder OUTPUT plane.
//Buffer acquisition strategy: first use each of the plane's buffers once
//(by index), afterwards block in dqBuffer(-1 = infinite) until the decoder
//releases one. The packet payload is memcpy'd, so the caller keeps
//ownership of it. pts (microseconds) is carried in the V4L2 timestamp.
//A zero-sized payload is the EOS marker and flips ctx->eos.
//Returns 0 on success, -1 on dequeue failure, -2 on queue failure,
//-3 when the packet exceeds chunk_size (packet dropped, decoder usable).
int nvmpi_decoder_put_packet(nvmpictx* ctx,nvPacket* packet)
{
	int ret;
	struct v4l2_buffer v4l2_buf;
	struct v4l2_plane planes[MAX_PLANES];
	NvBuffer *nvBuffer;

	//reject packets larger than the V4L2 input buffers before dequeuing
	//anything — copying would overflow the plane buffer. Distinct return
	//code: callers must treat this as invalid input data, not as a
	//transient/hardware failure (the FFmpeg wrapper maps it accordingly).
	if (packet->payload_size > ctx->chunk_size)
	{
		std::cerr << "[libnvmpi][E]: input packet (" << packet->payload_size
		          << " bytes) exceeds chunk_size (" << ctx->chunk_size
		          << "); dropping. Increase the chunk_size option." << std::endl;
		return -3;
	}

	memset(&v4l2_buf, 0, sizeof(v4l2_buf));
	memset(planes, 0, sizeof(planes));

	v4l2_buf.m.planes = planes;

	if (ctx->index < (int)ctx->dec->output_plane.getNumBuffers())
	{
		nvBuffer = ctx->dec->output_plane.getNthBuffer(ctx->index);
		v4l2_buf.index = ctx->index;
		ctx->index++;
	}
	else
	{
		ret = ctx->dec->output_plane.dqBuffer(v4l2_buf, &nvBuffer, NULL, -1);
		if (ret < 0)
		{
			cerr << "Error DQing buffer at output plane" << std::endl;
			return -1;
		}
	}

	memcpy(nvBuffer->planes[0].data,packet->payload,packet->payload_size);
	nvBuffer->planes[0].bytesused=packet->payload_size;
	v4l2_buf.m.planes[0].bytesused = nvBuffer->planes[0].bytesused;

	v4l2_buf.flags |= V4L2_BUF_FLAG_TIMESTAMP_COPY;
	v4l2_buf.timestamp.tv_sec = packet->pts / 1000000;
	v4l2_buf.timestamp.tv_usec = packet->pts % 1000000;

	ret = ctx->dec->output_plane.qBuffer(v4l2_buf, NULL);
	if (ret < 0)
	{
		std::cerr << "Error Qing buffer at output plane" << std::endl;
		ctx->index--;
		return -2;
	}

	if (v4l2_buf.m.planes[0].bytesused == 0)
	{
		ctx->eos=true;
		//std::cout << "Input file read complete" << std::endl; //TODO log it
	}

	return 0;
}

//Copy one filled DMA frame buffer into the caller's nvFrame planes.
//Per plane: map the dmabuf for CPU access, sync caches (device -> CPU),
//then copy line by line because the source pitch (hw-chosen) and the
//destination linesize (caller-chosen) usually differ; only
//frame_linedatasize valid bytes per line are copied. Unmaps when done.
//frame->payload[] must point at sufficiently large caller-owned memory.
int copyNvBufToFrame(nvmpictx* ctx, NVMPI_frameBuf *nvmpiBuf, nvFrame* frame)
{
	int ret;
	char *dataDst;
	char *dataSrc;

	for(unsigned int plane=0; plane<ctx->num_planes; plane++)
	{
#ifdef WITH_NVUTILS
		NvBufSurface *nvbuf_surf = nvmpiBuf->dst_dma_surface;
		ret = NvBufSurfaceMap(nvbuf_surf, 0, plane, NVBUF_MAP_READ_WRITE);
		NvBufSurfaceSyncForCpu (nvbuf_surf, 0, plane);
		dataSrc = (char *)nvbuf_surf->surfaceList[0].mappedAddr.addr[plane];
#else
		int dmabuf_fd = nvmpiBuf->dst_dma_fd;
		void *psrc_data;
		ret = NvBufferMemMap(dmabuf_fd, plane, NvBufferMem_Read_Write, &psrc_data);
		NvBufferMemSyncForCpu(dmabuf_fd, plane, &psrc_data);
		dataSrc = (char *)psrc_data;
#endif
		if(ret != 0)
		{
			fprintf(stderr, "NvBufferMap failed \n");
			return ret;
		}
		
		dataDst = (char *)frame->payload[plane];
		unsigned int &dstFrameLineSize = frame->linesize[plane];
		unsigned int &srcFrameLineSize = ctx->frame_linesize[plane];
		unsigned int &copySz = ctx->frame_linedatasize[plane];
		
		for (unsigned int i = 0; i < ctx->frame_height[plane]; i++)
		{
			memcpy(dataDst, dataSrc, copySz);
			dataDst += dstFrameLineSize;
			dataSrc += srcFrameLineSize;
		}
		
#ifdef WITH_NVUTILS
		NvBufSurfaceUnMap(nvbuf_surf, 0, plane);
#else
		NvBufferMemUnMap(dmabuf_fd, plane, &psrc_data);
#endif
	}
    return 0;
}

//Public API: fetch the next decoded frame.
//Non-blocking: returns -1 immediately when no filled frame is queued (the
//'wait' parameter is currently unused). On success the frame data is
//copied into the caller's buffers, the pts is set, and the pool buffer is
//immediately recycled to the "empty" queue — nothing internal is exposed
//to the caller, so there is no lifetime to manage.
int nvmpi_decoder_get_frame(nvmpictx* ctx,nvFrame* frame,bool wait)
{
	(void)wait; //blocking mode not implemented yet — see TODO.md
	int ret;
	NVMPI_frameBuf* fb = ctx->framePool->dqFilledBuf();
	if(!fb) return -1;
	
	ret = copyNvBufToFrame(ctx, fb, frame);
	frame->timestamp=fb->timestamp;
	
	//return buffer to pool
	ctx->framePool->qEmptyBuf(fb);
	
	return ret;
}

//Public API: shut the decoder down and free everything.
//
//Teardown order (each step depends on the previous):
//  1. Signal EOS and STREAMOFF capture plane — unblocks the capture thread.
//  2. Join the capture thread — guarantees no transforms are in flight.
//  3. Destroy the VIC transform session (legacy path only).
//  4. Stop the output (input-side) plane and release its V4L2 buffers.
//  5. Release CAPTURE-plane DMA buffers (decoder output side).
//  6. Destroy frame pool (VIC destination buffers) — safe because all
//     planes are stopped and no transforms can be pending.
//  7. Free the pool container, decoder device, and context.
//
//The handle is invalid after this call.
int nvmpi_decoder_close(nvmpictx* ctx)
{
	ctx->eos=true;
	ctx->dec->capture_plane.setStreamStatus(false);
	if (ctx->dec_capture_loop.joinable())
	{
		ctx->dec_capture_loop.join();
	}

#ifndef WITH_NVUTILS
	if (ctx->numberCaptureBuffers > 0)
		NvBufferSessionDestroy(ctx->session);
#endif

	ctx->dec->output_plane.setStreamStatus(false);
	ctx->dec->output_plane.deinitPlane();

	ctx->deinitDecoderCapturePlane();

	ctx->deinitFramePool();
	delete ctx->framePool;
	ctx->framePool = nullptr;

	delete ctx->dec;
	ctx->dec = nullptr;

	delete ctx;

	return 0;
}


