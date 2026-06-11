/*
 * nvUtils2NvBuf.h — compile-time shim between NVIDIA's two Jetson buffer
 * management APIs (libnvmpi layer).
 *
 * JetPack 4 ships the legacy "nvbuf_utils" API (NvBuffer... names); JetPack
 * 5+ replaces it with the NvUtils/NvBufSurface API (NvBufSurf and NVBUF_
 * names). CMakeLists.txt auto-detects which one is present (probe for
 * nvbufsurface.h) and defines WITH_NVUTILS for the new API.
 *
 * When WITH_NVUTILS is set, this header maps the legacy nvbuf_utils names
 * onto their NvBufSurface equivalents so that the rest of src/ can be
 * written once against the legacy spelling and compile against either API.
 * Calls with genuinely different signatures (e.g. NvBufferTransform vs
 * NvBufSurfTransform, memory map/sync) still need explicit #ifdef
 * WITH_NVUTILS branches at the call sites in nvmpi_dec.cpp / nvmpi_enc.cpp
 * and NVMPI_frameBuf.cpp — this shim only covers 1:1 renames.
 */
#if defined(WITH_NVUTILS)
/* NvUtils / NvBufSurface path (JetPack 5+) */
#include "nvbufsurface.h"
#include "nvbufsurftransform.h"
#include "NvBufSurface.h"
//upper bound on planes per surface, used to size per-plane arrays in nvmpictx
#define MAX_NUM_PLANES NVBUF_MAX_PLANES
//buffer lifetime: legacy destroy-by-fd maps to the NvBufSurf helper
#define NvBufferDestroy NvBufSurf::NvDestroy
//allocation parameter struct (width/height/layout/colorFormat/memType...)
#define NvBufferCreateParams NvBufSurf::NvCommonAllocateParams
//color formats: NV12 variants differ by colorimetry (601/709/2020) and
//luma range (standard vs ER = extended/full range)
#define NvBufferColorFormat_NV12 NVBUF_COLOR_FORMAT_NV12
#define NvBufferColorFormat_NV12_ER NVBUF_COLOR_FORMAT_NV12_ER
#define NvBufferColorFormat_NV12_709 NVBUF_COLOR_FORMAT_NV12_709
#define NvBufferColorFormat_NV12_709_ER NVBUF_COLOR_FORMAT_NV12_709_ER
#define NvBufferColorFormat_NV12_2020 NVBUF_COLOR_FORMAT_NV12_2020
#define NvBufferColorFormat_YUV420 NVBUF_COLOR_FORMAT_YUV420
//#define NvBufferColorFormat_ABGR32 NVBUF_COLOR_FORMAT_BGRA
//NVBUF_COLOR_FORMAT_BGR - BGR-8-8-8 single plane. /nvbufsurface API only/
//NVBUF_COLOR_FORMAT_B8_G8_R8 -  BGR- unsigned 8-bit multiplanar plane. /nvbufsurface API only/
//memory layouts: Pitch = CPU-readable linear, BlockLinear = decoder-native
#define NvBufferLayout_Pitch NVBUF_LAYOUT_PITCH
#define NvBufferLayout_BlockLinear NVBUF_LAYOUT_BLOCK_LINEAR
//VIC transform (scale/convert/crop) parameter and rect types + flags;
//"Smart" maps to the high-quality Algo3 interpolation filter
#define NvBufferTransformParams NvBufSurfTransformParams
#define NvBufferRect NvBufSurfTransformRect
#define NVBUFFER_TRANSFORM_FILTER NVBUFSURF_TRANSFORM_FILTER
#define NvBufferTransform_None NvBufSurfTransform_None
#define NvBufferTransform_Filter_Smart NvBufSurfTransformInter_Algo3
#define NvBufferTransform_Filter_Nearest NvBufSurfTransformInter_Nearest
#define NvBufferParams NvBufSurfTransform
#define NvBufferColorFormat NvBufSurfaceColorFormat

//#define NvBuffer2Raw(dmabuf, plane, out_width, out_height, ptr)
#else
/* Legacy nvbuf_utils path (JetPack 4 and earlier): the NvBuffer* names used
   throughout src/ are the real API here, so just include its header. */
#include "nvbuf_utils.h"
#endif
