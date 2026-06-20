#include "nvmpi_dec_internal.h"

//Map the colorspace/quantization reported by the decoder on its CAPTURE
//plane to the matching NvBuffer NV12 color format variant (BT.601/709/2020,
//standard vs extended luma range). Used when allocating the CAPTURE-plane
//DMA buffers so the subsequent VIC transform interprets colors correctly.
NvBufferColorFormat getNvColorFormatFromV4l2Format(v4l2_format &format, bool want_10bit)
{
#ifdef WITH_NVUTILS
	//10-bit (P010) output: pick the matching NV12_10LE colorimetry variant.
	//These constants exist only in the NvUtils API, which is why P010 is
	//gated on WITH_NVUTILS at the decoder init layer.
	if (want_10bit)
	{
		switch (format.fmt.pix_mp.colorspace)
		{
			case V4L2_COLORSPACE_REC709:
				return (format.fmt.pix_mp.quantization == V4L2_QUANTIZATION_DEFAULT)
					? NvBufferColorFormat_NV12_10LE_709 : NvBufferColorFormat_NV12_10LE_709_ER;
			case V4L2_COLORSPACE_BT2020:
				return NvBufferColorFormat_NV12_10LE_2020;
			case V4L2_COLORSPACE_SMPTE170M:
			default:
				return (format.fmt.pix_mp.quantization == V4L2_QUANTIZATION_DEFAULT)
					? NvBufferColorFormat_NV12_10LE : NvBufferColorFormat_NV12_10LE_ER;
		}
	}
#else
	(void)want_10bit;
#endif
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
	cParams.colorFormat = getNvColorFormatFromV4l2Format(format, out_pixfmt == NV_PIX_P010);
	cParams.width = coded_width;
	cParams.height = coded_height;
	cParams.layout = NvBufferLayout_BlockLinear;
#ifdef WITH_NVUTILS
	cParams.memType = NVBUF_MEM_SURFACE_ARRAY;
	cParams.memtag = NvBufSurfaceTag_VIDEO_DEC;

	//NvUtils path: allocate all buffers in one call, then resolve the
	//NvBufSurface view of each fd for use with NvBufSurfTransform.
	ret = NvBufSurf::NvAllocate(&cParams, numberCaptureBuffers, dmaBufferFileDescriptor);
	TEST_ERROR(ret < 0, "Failed to create buffers", ret);
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
