#pragma once

#include "nvmpi.h"
#include "NvVideoEncoder.h"
#include "nvUtils2NvBuf.h"
#include "NVMPI_bufPool.hpp"
#include <fcntl.h>
#include <malloc.h>
#include <vector>
#include "nvmpi_log.h"
#include <thread>
#include <unistd.h>
#include <memory>
#include <atomic>

#define MAX_BUFFERS 32
//Error-check macro for encoder init: logs to stderr and jumps to the
//function's cleanup label on failure. Every call site must be inside a
//function that has a `cleanup:` label and an `int ret` in scope.
#define TEST_ERROR(condition, message, errorCode)         \
	if (condition) {                                     \
		NVMPI_LOG(NVMPI_LOG_ERROR, "%s (code=%d)",       \
		          message, (int)(errorCode));             \
		goto cleanup;                                    \
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
	bool insert_aud;               //insert Access Unit Delimiter NALs
	bool enable_cabac;             //enable CABAC entropy coding (H.264 only)
	bool max_perf;                 //enable max performance mode
	uint32_t poc_type;             //H.264 picture order count type (0=default, 2=low-latency)
	bool enable_extended_colorformat;
	bool enableLossless;           //constant QP 0 + High 4:4:4 profile (H.264)
	bool blocking_mode;            //true: use NvVideoEncoder's DQ thread (only mode implemented)
	/* Set to true in nvmpi_create_encoder() after startDQThread succeeds;
	 * checked in nvmpi_encoder_close() to avoid calling stopDQThread on
	 * an un-started thread (pthread_join on uninitialized thread = UB).
	 * Also used by the cleanup path when nvmpi_create_encoder fails
	 * after STREAMON but before the DQ thread is started. */
	bool dq_thread_started{false};

	/* Cross-thread flags — atomic to avoid data races between the user
	 * thread (put_frame/get_packet) and the capture-plane DQ callback.
	 * capPlaneGotEOS: written by DQ callback (nvmpi_enc_output.cpp),
	 *   read by user thread in nvmpi_encoder_get_packet().
	 * flushing: written by user thread in nvmpi_encoder_put_frame(),
	 *   read by user thread in get_packet; combined with capPlaneGotEOS
	 *   so both must use acquire/release ordering for coherence. */
	std::atomic<bool> capPlaneGotEOS{false};
	std::atomic<bool> flushing{false};

	/* Blocking-wait ceiling for nvmpi_encoder_get_packet() when wait=true.
	 * Mirrors the decoder's wait_timeout_ms (default 500ms). */
	unsigned int wait_timeout_ms{500};

	enum v4l2_mpeg_video_bitrate_mode ratecontrol; //CBR or VBR
	/* V4L2 level enum — uint32_t so it can hold either
	 * v4l2_mpeg_video_h264_level or v4l2_mpeg_video_hevc_level. */
	uint32_t level;
	enum v4l2_enc_hw_preset_type hw_preset_type;   //speed/quality preset

	/* NVIDIA V4L2 encoder device wrapper. unique_ptr guarantees cleanup
	 * on all exit paths, preventing leaks from partial init failures.
	 * Allocated in nvmpi_create_encoder(); released in nvmpi_encoder_close()
	 * via reset() after the DQ thread is joined. */
	std::unique_ptr<NvVideoEncoder> enc;
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
