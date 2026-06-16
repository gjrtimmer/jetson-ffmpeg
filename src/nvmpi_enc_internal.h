#pragma once

#include "nvmpi.h"
#include "NvVideoEncoder.h"
#include "nvUtils2NvBuf.h"
#include "NVMPI_bufPool.hpp"
#include <fcntl.h>
#include <malloc.h>
#include <vector>
#include <iostream>
#include <thread>
#include <unistd.h>

#define MAX_BUFFERS 32
//Error reporting helper: logs to stderr but does NOT abort or return;
//setup continues best-effort (errorCode is unused).
#define TEST_ERROR(condition, message, errorCode)    \
	if (condition)                               \
{                                                    \
	std::cerr<< message;                         \
}

//Compile-time choice of OUTPUT-plane (raw input) memory type:
//MMAP = driver-allocated buffers mapped into userspace (current default),
//DMA  = self-allocated NvBufSurface dmabufs (alternate path, NvUtils only).
#define OUTPLANE_MEMTYPE_MMAP 0
#define OUTPLANE_MEMTYPE_DMA 1
#define OUTPLANE_MEMTYPE OUTPLANE_MEMTYPE_MMAP

using namespace std;

//Encoder context behind the opaque nvmpictx* handle of the public API
//(a different struct than the decoder's nvmpictx in nvmpi_dec_internal.h — the
//two are never mixed). Most fields mirror nvEncParam after translation to
//V4L2 enums; shared between the user thread and the capture DQ thread.
struct nvmpictx
{
	uint32_t index;                //next OUTPUT-plane buffer index until all used once
	uint32_t width;
	uint32_t height;
	uint32_t profile;              //V4L2 H.264/H.265 profile enum value
	uint32_t bitrate;              //target bitrate (bit/s)
	uint32_t peak_bitrate;         //VBR peak bitrate (bit/s)
	uint32_t raw_pixfmt;           //V4L2 fourcc of the raw input frames
	uint32_t encoder_pixfmt;       //V4L2 fourcc of the compressed output (H264/H265)
	uint32_t iframe_interval;
	uint32_t idr_interval;
	uint32_t fps_n;
	uint32_t fps_d;
	uint32_t qmax;
	uint32_t qmin;
	uint32_t num_b_frames;
	uint32_t num_reference_frames;
	uint32_t vbv_buffer_size; //virtual buffer size of the encoder
	uint32_t packets_num;          //V4L2 buffer count per plane (param->capture_num)

	bool insert_sps_pps_at_idr;
	bool insert_vui;               //embed VUI timing_info (fps) in the bitstream
	bool max_perf;                 //enable max performance mode
	uint32_t poc_type;             //H.264 picture order count type (0=default, 2=low-latency)
	bool enable_extended_colorformat;
	bool enableLossless;           //constant QP 0 + High 4:4:4 profile (H.264)
	bool blocking_mode;            //true: use NvVideoEncoder's DQ thread (only mode implemented)
	bool capPlaneGotEOS;           //set by the DQ callback on the zero-byte EOS buffer
	bool flushing;                 //set once a NULL frame (EOS) was submitted

	enum v4l2_mpeg_video_bitrate_mode ratecontrol; //CBR or VBR
	enum v4l2_mpeg_video_h264_level level;
	enum v4l2_enc_hw_preset_type hw_preset_type;   //speed/quality preset

	NvVideoEncoder *enc;           //NVIDIA V4L2 encoder device wrapper
	//producer/consumer pool of caller-allocated packets: DQ thread fills,
	//user thread consumes and recycles
	NVMPI_bufPool<nvPacket*>* pktPool;
	int *output_plane_fd; //array to store dmabuf fd's
};

//Forward declarations for functions defined across the split encoder files.
//encoder_capture_plane_dq_callback: defined in nvmpi_enc_output.cpp; registered
//with NvVideoEncoder's capture-plane DQ thread in nvmpi_create_encoder().
bool encoder_capture_plane_dq_callback(struct v4l2_buffer *v4l2_buf, NvBuffer *buffer, NvBuffer *shared_buffer, void *arg);
//copyFrameToNvBuf: defined in nvmpi_enc_api.cpp; copies a caller raw frame
//into a V4L2 OUTPUT-plane NvBuffer (used by nvmpi_encoder_put_frame()).
int copyFrameToNvBuf(nvFrame* frame, NvBuffer& buffer);
