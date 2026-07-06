#!/bin/bash
#path to root ffmpeg sources dir must be passed as first arg to the script

# Resolve the repository root from this script's location so ffpatch.sh can be
# invoked from any working directory (it lives in <repo>/scripts/).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

FF_DIR_ROOT=${1}
FF_DIR_LIBAVCODEC=${FF_DIR_ROOT}"/libavcodec"
FF_DIR_LIBAVUTIL=${FF_DIR_ROOT}"/libavutil"
FF_DIR_LIBAVFILTER=${FF_DIR_ROOT}"/libavfilter"
FF_FILE_CONFIGURE=${FF_DIR_ROOT}"/configure"
FF_FILE_LIBAVCODEC_MAKEFILE=${FF_DIR_LIBAVCODEC}"/Makefile"
FF_FILE_LIBAVCODEC_ALLCODECSC=${FF_DIR_LIBAVCODEC}"/allcodecs.c"
FF_FILE_LIBAVCODEC_VERSIONH=${FF_DIR_LIBAVCODEC}"/version.h"
FF_FILE_LIBAVCODEC_VERSIONMAJORH=${FF_DIR_LIBAVCODEC}"/version_major.h"
FF_FILE_LIBAVUTIL_MAKEFILE=${FF_DIR_LIBAVUTIL}"/Makefile"
FF_FILE_LIBAVUTIL_HWCONTEXTH=${FF_DIR_LIBAVUTIL}"/hwcontext.h"
FF_FILE_LIBAVUTIL_HWCONTEXTC=${FF_DIR_LIBAVUTIL}"/hwcontext.c"
FF_FILE_LIBAVUTIL_HWCONTEXT_INTERNALH=${FF_DIR_LIBAVUTIL}"/hwcontext_internal.h"
FF_FILE_LIBAVFILTER_MAKEFILE=${FF_DIR_LIBAVFILTER}"/Makefile"
FF_FILE_LIBAVFILTER_ALLFILTERSC=${FF_DIR_LIBAVFILTER}"/allfilters.c"
FF_LIBAVCODEC_VERSION_MAJOR=0
BKP_DIR="${REPO_ROOT}/bkp"
BKP_FILE_CONFIGURE=${BKP_DIR}/configure
BKP_FILE_LIBAVCODEC_MAKEFILE=${BKP_DIR}/Makefile
BKP_FILE_LIBAVCODEC_ALLCODECSC=${BKP_DIR}/allcodecs.c
BKP_FILE_LIBAVUTIL_MAKEFILE=${BKP_DIR}/libavutil_Makefile
BKP_FILE_LIBAVUTIL_HWCONTEXTH=${BKP_DIR}/hwcontext.h
BKP_FILE_LIBAVUTIL_HWCONTEXTC=${BKP_DIR}/hwcontext.c
BKP_FILE_LIBAVUTIL_HWCONTEXT_INTERNALH=${BKP_DIR}/hwcontext_internal.h
BKP_FILE_LIBAVFILTER_MAKEFILE=${BKP_DIR}/libavfilter_Makefile
BKP_FILE_LIBAVFILTER_ALLFILTERSC=${BKP_DIR}/libavfilter_allfilters.c

if [ ! -d "$FF_DIR_ROOT" ]; then
	echo "[E]: $FF_DIR_ROOT does not exist or not a directory. You must specify path to ffmpeg sources directory."
	exit 1
fi

if [ ! -d "$FF_DIR_LIBAVCODEC" ]; then
	echo "[E]: $FF_DIR_LIBAVCODEC does not exist or not a directory. Your ffmpeg sources are not complete or this ffmpeg version not supported by the script yet."
	exit 1
fi

if [ ! -f "$FF_FILE_CONFIGURE" ]; then
	echo "[E]: $FF_FILE_CONFIGURE does not exist or not a file. Your ffmpeg sources are not complete or this ffmpeg version not supported by the script yet."
	exit 1
fi

if [ ! -f "$FF_FILE_LIBAVCODEC_MAKEFILE" ]; then
	echo "[E]: $FF_FILE_LIBAVCODEC_MAKEFILE does not exist or not a file. Your ffmpeg sources are not complete or this ffmpeg version not supported by the script yet."
	exit 1
fi

if [ ! -f "$FF_FILE_LIBAVCODEC_ALLCODECSC" ]; then
	echo "[E]: $FF_FILE_LIBAVCODEC_ALLCODECSC does not exist or not a file. Your ffmpeg sources are not complete or this ffmpeg version not supported by the script yet."
	exit 1
fi

if [ ! -d "$FF_DIR_LIBAVUTIL" ]; then
	echo "[E]: $FF_DIR_LIBAVUTIL does not exist or not a directory. Your ffmpeg sources are not complete or this ffmpeg version not supported by the script yet."
	exit 1
fi

if [ ! -d "$FF_DIR_LIBAVFILTER" ]; then
	echo "[E]: $FF_DIR_LIBAVFILTER does not exist or not a directory. Your ffmpeg sources are not complete or this ffmpeg version not supported by the script yet."
	exit 1
fi

if [ ! -f "$FF_FILE_LIBAVUTIL_HWCONTEXTH" ]; then
	echo "[E]: $FF_FILE_LIBAVUTIL_HWCONTEXTH does not exist. Your ffmpeg sources are not complete."
	exit 1
fi

#read and check ffmpeg libavcodec version 
if [ -f "$FF_FILE_LIBAVCODEC_VERSIONMAJORH" ]; then
	FF_LIBAVCODEC_VERSION_MAJOR=$(grep '#define LIBAVCODEC_VERSION_MAJOR' $FF_FILE_LIBAVCODEC_VERSIONMAJORH | awk '{printf "%d",$3}')
elif [ -f "$FF_FILE_LIBAVCODEC_VERSIONH" ]; then
	FF_LIBAVCODEC_VERSION_MAJOR=$(grep '#define LIBAVCODEC_VERSION_MAJOR' $FF_FILE_LIBAVCODEC_VERSIONH | awk '{printf "%d",$3}')
else
	echo "[E]: Can't find ffmpeg libavcodec version file. Your ffmpeg sources are not complete or this ffmpeg version not supported by the script yet."
	exit 1
fi
if [ "$FF_LIBAVCODEC_VERSION_MAJOR" -eq 0 ]; then
	echo "[E]: Can't read ffmpeg libavcodec version. Your ffmpeg sources are not complete or this ffmpeg version not supported by the script yet."
	exit 1
fi

#jetson-ffmpeg 3.x requires libavcodec >= 60 (FFmpeg 6.0+); FFmpeg 4.x/5.x
#used AVCodec instead of FFCodec and are no longer supported.
if [ "$FF_LIBAVCODEC_VERSION_MAJOR" -lt 60 ]; then
	echo "[E]: FFmpeg with libavcodec $FF_LIBAVCODEC_VERSION_MAJOR is not supported. jetson-ffmpeg 3.x requires FFmpeg 6.0+ (libavcodec >= 60). Use jetson-ffmpeg v2.x for FFmpeg 4.x/5.x."
	exit 1
fi

rm -rf "$BKP_DIR" 2>&1 > /dev/null
if ! mkdir "$BKP_DIR" 2>&1 > /dev/null; then
	echo "Can not create backup dir $BKP_DIR"
	exit 1
fi

cp "$FF_FILE_CONFIGURE" "$BKP_FILE_CONFIGURE"
cp "$FF_FILE_LIBAVCODEC_MAKEFILE" "$BKP_FILE_LIBAVCODEC_MAKEFILE"
cp "$FF_FILE_LIBAVCODEC_ALLCODECSC" "$BKP_FILE_LIBAVCODEC_ALLCODECSC"
cp "$FF_FILE_LIBAVUTIL_MAKEFILE" "$BKP_FILE_LIBAVUTIL_MAKEFILE"
cp "$FF_FILE_LIBAVUTIL_HWCONTEXTH" "$BKP_FILE_LIBAVUTIL_HWCONTEXTH"
cp "$FF_FILE_LIBAVUTIL_HWCONTEXTC" "$BKP_FILE_LIBAVUTIL_HWCONTEXTC"
cp "$FF_FILE_LIBAVUTIL_HWCONTEXT_INTERNALH" "$BKP_FILE_LIBAVUTIL_HWCONTEXT_INTERNALH"
cp "$FF_FILE_LIBAVFILTER_MAKEFILE" "$BKP_FILE_LIBAVFILTER_MAKEFILE"
cp "$FF_FILE_LIBAVFILTER_ALLFILTERSC" "$BKP_FILE_LIBAVFILTER_ALLFILTERSC"

################## MODIFY configure ############################
function path_ff_configure ()
{
#add nvmpi to ffmpeg configure show_help() function.
if ! grep -q -- '--enable-nvmpi           enable nvmpi code \[no\]' "$BKP_FILE_CONFIGURE"; then
	cp "$BKP_FILE_CONFIGURE" "$BKP_FILE_CONFIGURE.1"
	sed -i '/--disable-videotoolbox/a \ \ --enable-nvmpi           enable nvmpi code [no]' "$BKP_FILE_CONFIGURE"
	if cmp "$BKP_FILE_CONFIGURE" "$BKP_FILE_CONFIGURE.1"; then return 1; fi;
fi

#add nvmpi to ffmpeg configure HWACCEL_LIBRARY_LIST.
if ! sed -n '/HWACCEL_LIBRARY_LIST="/,/^"/p' "$BKP_FILE_CONFIGURE" | grep -qx '[[:space:]]*nvmpi'; then
	cp "$BKP_FILE_CONFIGURE" "$BKP_FILE_CONFIGURE.1"
	sed -i '/HWACCEL_LIBRARY_LIST="/a \ \ \ \ nvmpi' "$BKP_FILE_CONFIGURE"
	if cmp "$BKP_FILE_CONFIGURE" "$BKP_FILE_CONFIGURE.1"; then return 1; fi;
fi

#add nvmpi avc/h264 deps. insert before h264_nvenc_encoder_deps="nvenc"
if ! grep -q 'h264_nvmpi_encoder_deps' "$BKP_FILE_CONFIGURE"; then
	cp "$BKP_FILE_CONFIGURE" "$BKP_FILE_CONFIGURE.1"
	sed -i '/h264_nvenc_encoder_deps="nvenc"/i h264_nvmpi_encoder_deps="nvmpi"\nh264_nvmpi_decoder_deps="nvmpi"\nh264_nvmpi_decoder_select="h264_mp4toannexb_bsf"' "$BKP_FILE_CONFIGURE"
	if cmp "$BKP_FILE_CONFIGURE" "$BKP_FILE_CONFIGURE.1"; then return 1; fi;
fi

#add nvmpi hevc/h265 deps. insert before hevc_nvenc_encoder_deps="nvenc"
if ! grep -q 'hevc_nvmpi_encoder_deps' "$BKP_FILE_CONFIGURE"; then
	cp "$BKP_FILE_CONFIGURE" "$BKP_FILE_CONFIGURE.1"
	sed -i '/hevc_nvenc_encoder_deps="nvenc"/i hevc_nvmpi_encoder_deps="nvmpi"\nhevc_nvmpi_decoder_deps="nvmpi"\nhevc_nvmpi_decoder_select="hevc_mp4toannexb_bsf"' "$BKP_FILE_CONFIGURE"
	if cmp "$BKP_FILE_CONFIGURE" "$BKP_FILE_CONFIGURE.1"; then return 1; fi;
fi

#add nvmpi mpeg2 deps. insert after mpeg2_cuvid_decoder_deps="cuvid"
if ! grep -q 'mpeg2_nvmpi_decoder_deps' "$BKP_FILE_CONFIGURE"; then
	cp "$BKP_FILE_CONFIGURE" "$BKP_FILE_CONFIGURE.1"
	sed -i '/mpeg2_cuvid_decoder_deps="cuvid"/a mpeg2_nvmpi_decoder_deps="nvmpi"' "$BKP_FILE_CONFIGURE"
	if cmp "$BKP_FILE_CONFIGURE" "$BKP_FILE_CONFIGURE.1"; then return 1; fi;
fi

#add nvmpi mpeg4 deps. insert after mpeg4_cuvid_decoder_deps="cuvid"
if ! grep -q 'mpeg4_nvmpi_decoder_deps' "$BKP_FILE_CONFIGURE"; then
	cp "$BKP_FILE_CONFIGURE" "$BKP_FILE_CONFIGURE.1"
	sed -i '/mpeg4_cuvid_decoder_deps="cuvid"/a mpeg4_nvmpi_decoder_deps="nvmpi"' "$BKP_FILE_CONFIGURE"
	if cmp "$BKP_FILE_CONFIGURE" "$BKP_FILE_CONFIGURE.1"; then return 1; fi;
fi

#add nvmpi vp8 deps. insert after vp8_cuvid_decoder_deps="cuvid"
if ! grep -q 'vp8_nvmpi_decoder_deps' "$BKP_FILE_CONFIGURE"; then
	cp "$BKP_FILE_CONFIGURE" "$BKP_FILE_CONFIGURE.1"
	sed -i '/vp8_cuvid_decoder_deps="cuvid"/a vp8_nvmpi_decoder_deps="nvmpi"' "$BKP_FILE_CONFIGURE"
	if cmp "$BKP_FILE_CONFIGURE" "$BKP_FILE_CONFIGURE.1"; then return 1; fi;
fi

#add nvmpi vp9 deps. insert after vp9_cuvid_decoder_deps="cuvid"
if ! grep -q 'vp9_nvmpi_decoder_deps' "$BKP_FILE_CONFIGURE"; then
	cp "$BKP_FILE_CONFIGURE" "$BKP_FILE_CONFIGURE.1"
	sed -i '/vp9_cuvid_decoder_deps="cuvid"/a vp9_nvmpi_decoder_deps="nvmpi"' "$BKP_FILE_CONFIGURE"
	if cmp "$BKP_FILE_CONFIGURE" "$BKP_FILE_CONFIGURE.1"; then return 1; fi;
fi

#add nvmpi mjpeg deps. insert after mjpeg_cuvid_decoder_deps="cuvid"
if ! grep -q 'mjpeg_nvmpi_decoder_deps' "$BKP_FILE_CONFIGURE"; then
	cp "$BKP_FILE_CONFIGURE" "$BKP_FILE_CONFIGURE.1"
	sed -i '/mjpeg_cuvid_decoder_deps="cuvid"/a mjpeg_nvmpi_decoder_deps="nvmpi"' "$BKP_FILE_CONFIGURE"
	if cmp "$BKP_FILE_CONFIGURE" "$BKP_FILE_CONFIGURE.1"; then return 1; fi;
fi

#add nvmpi mjpeg encoder deps. insert after mjpeg_nvmpi_decoder_deps
if ! grep -q 'mjpeg_nvmpi_encoder_deps' "$BKP_FILE_CONFIGURE"; then
	cp "$BKP_FILE_CONFIGURE" "$BKP_FILE_CONFIGURE.1"
	sed -i '/mjpeg_nvmpi_decoder_deps="nvmpi"/a mjpeg_nvmpi_encoder_deps="nvmpi"' "$BKP_FILE_CONFIGURE"
	if cmp "$BKP_FILE_CONFIGURE" "$BKP_FILE_CONFIGURE.1"; then return 1; fi;
fi

#insert before enabled libx264 line.
#nvmpi uses dlopen (dynlink_nvmpi.h) — no link-time dependency on libnvmpi.so.
#Only -ldl is needed for dlopen/dlsym/dlclose.
if ! grep -q 'enabled nvmpi' "$BKP_FILE_CONFIGURE"; then
	cp "$BKP_FILE_CONFIGURE" "$BKP_FILE_CONFIGURE.1"
	sed -i '/enabled libx264/i enabled nvmpi		  && add_extralibs -ldl' "$BKP_FILE_CONFIGURE"
	if cmp "$BKP_FILE_CONFIGURE" "$BKP_FILE_CONFIGURE.1"; then return 1; fi;
fi

return 0;
}
################## MODIFY configure ############################

################## MODIFY libavcodec/Makefile ############################
function path_ff_libavcodec_Makefile ()
{
#add nvmpi avc/h264 encoder and decoder
if ! grep -q 'CONFIG_H264_NVMPI_DECODER' "$BKP_FILE_LIBAVCODEC_MAKEFILE"; then
	cp "$BKP_FILE_LIBAVCODEC_MAKEFILE" "$BKP_FILE_LIBAVCODEC_MAKEFILE.1"
	sed -i '/OBJS-\$(CONFIG_H264_NVENC_ENCODER)/i OBJS-\$(CONFIG_H264_NVMPI_DECODER)      += nvmpi_dec.o\nOBJS-$(CONFIG_H264_NVMPI_ENCODER)      += nvmpi_enc.o nvmpi_enc_runtime.o' "$BKP_FILE_LIBAVCODEC_MAKEFILE"
	if cmp "$BKP_FILE_LIBAVCODEC_MAKEFILE" "$BKP_FILE_LIBAVCODEC_MAKEFILE.1"; then return 1; fi;
fi

#add nvmpi hevc/h265 encoder and decoder
if ! grep -q 'CONFIG_HEVC_NVMPI_DECODER' "$BKP_FILE_LIBAVCODEC_MAKEFILE"; then
	cp "$BKP_FILE_LIBAVCODEC_MAKEFILE" "$BKP_FILE_LIBAVCODEC_MAKEFILE.1"
	sed -i '/OBJS-\$(CONFIG_HEVC_NVENC_ENCODER)/i OBJS-\$(CONFIG_HEVC_NVMPI_DECODER)      += nvmpi_dec.o\nOBJS-$(CONFIG_HEVC_NVMPI_ENCODER)      += nvmpi_enc.o nvmpi_enc_runtime.o' "$BKP_FILE_LIBAVCODEC_MAKEFILE"
	if cmp "$BKP_FILE_LIBAVCODEC_MAKEFILE" "$BKP_FILE_LIBAVCODEC_MAKEFILE.1"; then return 1; fi;
fi

#add nvmpi mpeg2 encoder and decoder
if ! grep -q 'CONFIG_MPEG2_NVMPI_DECODER' "$BKP_FILE_LIBAVCODEC_MAKEFILE"; then
	cp "$BKP_FILE_LIBAVCODEC_MAKEFILE" "$BKP_FILE_LIBAVCODEC_MAKEFILE.1"
	sed -i '/OBJS-\$(CONFIG_MPEG2_CUVID_DECODER)/i OBJS-\$(CONFIG_MPEG2_NVMPI_DECODER)      += nvmpi_dec.o' "$BKP_FILE_LIBAVCODEC_MAKEFILE"
	if cmp "$BKP_FILE_LIBAVCODEC_MAKEFILE" "$BKP_FILE_LIBAVCODEC_MAKEFILE.1"; then return 1; fi;
fi

#add nvmpi mpeg4 encoder and decoder
if ! grep -q 'CONFIG_MPEG4_NVMPI_DECODER' "$BKP_FILE_LIBAVCODEC_MAKEFILE"; then
	cp "$BKP_FILE_LIBAVCODEC_MAKEFILE" "$BKP_FILE_LIBAVCODEC_MAKEFILE.1"
	sed -i '/OBJS-\$(CONFIG_MPEG4_CUVID_DECODER)/i OBJS-\$(CONFIG_MPEG4_NVMPI_DECODER)      += nvmpi_dec.o' "$BKP_FILE_LIBAVCODEC_MAKEFILE"
	if cmp "$BKP_FILE_LIBAVCODEC_MAKEFILE" "$BKP_FILE_LIBAVCODEC_MAKEFILE.1"; then return 1; fi;
fi

#add nvmpi vp8 encoder and decoder
if ! grep -q 'CONFIG_VP8_NVMPI_DECODER' "$BKP_FILE_LIBAVCODEC_MAKEFILE"; then
	cp "$BKP_FILE_LIBAVCODEC_MAKEFILE" "$BKP_FILE_LIBAVCODEC_MAKEFILE.1"
	sed -i '/OBJS-\$(CONFIG_VP8_CUVID_DECODER)/i OBJS-\$(CONFIG_VP8_NVMPI_DECODER)      += nvmpi_dec.o' "$BKP_FILE_LIBAVCODEC_MAKEFILE"
	if cmp "$BKP_FILE_LIBAVCODEC_MAKEFILE" "$BKP_FILE_LIBAVCODEC_MAKEFILE.1"; then return 1; fi;
fi

#add nvmpi vp9 encoder and decoder
if ! grep -q 'CONFIG_VP9_NVMPI_DECODER' "$BKP_FILE_LIBAVCODEC_MAKEFILE"; then
	cp "$BKP_FILE_LIBAVCODEC_MAKEFILE" "$BKP_FILE_LIBAVCODEC_MAKEFILE.1"
	sed -i '/OBJS-\$(CONFIG_VP9_CUVID_DECODER)/i OBJS-\$(CONFIG_VP9_NVMPI_DECODER)      += nvmpi_dec.o' "$BKP_FILE_LIBAVCODEC_MAKEFILE"
	if cmp "$BKP_FILE_LIBAVCODEC_MAKEFILE" "$BKP_FILE_LIBAVCODEC_MAKEFILE.1"; then return 1; fi;
fi

#add nvmpi mjpeg decoder
if ! grep -q 'CONFIG_MJPEG_NVMPI_DECODER' "$BKP_FILE_LIBAVCODEC_MAKEFILE"; then
	cp "$BKP_FILE_LIBAVCODEC_MAKEFILE" "$BKP_FILE_LIBAVCODEC_MAKEFILE.1"
	sed -i '/OBJS-\$(CONFIG_MJPEG_CUVID_DECODER)/i OBJS-\$(CONFIG_MJPEG_NVMPI_DECODER)      += nvmpi_dec_mjpeg.o' "$BKP_FILE_LIBAVCODEC_MAKEFILE"
	if cmp "$BKP_FILE_LIBAVCODEC_MAKEFILE" "$BKP_FILE_LIBAVCODEC_MAKEFILE.1"; then return 1; fi;
fi

#add nvmpi mjpeg encoder
if ! grep -q 'CONFIG_MJPEG_NVMPI_ENCODER' "$BKP_FILE_LIBAVCODEC_MAKEFILE"; then
	cp "$BKP_FILE_LIBAVCODEC_MAKEFILE" "$BKP_FILE_LIBAVCODEC_MAKEFILE.1"
	sed -i '/OBJS-\$(CONFIG_MJPEG_NVMPI_DECODER)/a OBJS-\$(CONFIG_MJPEG_NVMPI_ENCODER)      += nvmpi_enc_jpeg.o' "$BKP_FILE_LIBAVCODEC_MAKEFILE"
	if cmp "$BKP_FILE_LIBAVCODEC_MAKEFILE" "$BKP_FILE_LIBAVCODEC_MAKEFILE.1"; then return 1; fi;
fi

return 0;
}
################## MODIFY libavcodec/Makefile ############################

################## MODIFY libavcodec/allcodecs.c ############################
function path_ff_libavcodec_allcodecsc ()
{
FF_CODEC_INTERFACE="extern const FFCodec"

#add nvmpi avc/h264 encoder and decoder
if ! grep -q 'ff_h264_nvmpi_decoder' "$BKP_FILE_LIBAVCODEC_ALLCODECSC"; then
	cp "$BKP_FILE_LIBAVCODEC_ALLCODECSC" "$BKP_FILE_LIBAVCODEC_ALLCODECSC.1"
	sed -i "/$FF_CODEC_INTERFACE ff_h264_decoder;/a $FF_CODEC_INTERFACE ff_h264_nvmpi_decoder;\n$FF_CODEC_INTERFACE ff_h264_nvmpi_encoder;" "$BKP_FILE_LIBAVCODEC_ALLCODECSC"
	if cmp "$BKP_FILE_LIBAVCODEC_ALLCODECSC" "$BKP_FILE_LIBAVCODEC_ALLCODECSC.1"; then return 1; fi;
fi

#add nvmpi hevc/h265 encoder and decoder
if ! grep -q 'ff_hevc_nvmpi_decoder' "$BKP_FILE_LIBAVCODEC_ALLCODECSC"; then
	cp "$BKP_FILE_LIBAVCODEC_ALLCODECSC" "$BKP_FILE_LIBAVCODEC_ALLCODECSC.1"
	sed -i "/$FF_CODEC_INTERFACE ff_hevc_decoder;/a $FF_CODEC_INTERFACE ff_hevc_nvmpi_decoder;\n$FF_CODEC_INTERFACE ff_hevc_nvmpi_encoder;" "$BKP_FILE_LIBAVCODEC_ALLCODECSC"
	if cmp "$BKP_FILE_LIBAVCODEC_ALLCODECSC" "$BKP_FILE_LIBAVCODEC_ALLCODECSC.1"; then return 1; fi;
fi

#add nvmpi mpeg2 encoder and decoder
if ! grep -q 'ff_mpeg2_nvmpi_decoder' "$BKP_FILE_LIBAVCODEC_ALLCODECSC"; then
	cp "$BKP_FILE_LIBAVCODEC_ALLCODECSC" "$BKP_FILE_LIBAVCODEC_ALLCODECSC.1"
	sed -i "/$FF_CODEC_INTERFACE ff_mpeg2video_decoder;/a $FF_CODEC_INTERFACE ff_mpeg2_nvmpi_decoder;" "$BKP_FILE_LIBAVCODEC_ALLCODECSC"
	if cmp "$BKP_FILE_LIBAVCODEC_ALLCODECSC" "$BKP_FILE_LIBAVCODEC_ALLCODECSC.1"; then return 1; fi;
fi

#add nvmpi mpeg4 encoder and decoder
if ! grep -q 'ff_mpeg4_nvmpi_decoder' "$BKP_FILE_LIBAVCODEC_ALLCODECSC"; then
	cp "$BKP_FILE_LIBAVCODEC_ALLCODECSC" "$BKP_FILE_LIBAVCODEC_ALLCODECSC.1"
	sed -i "/$FF_CODEC_INTERFACE ff_mpeg4_decoder;/a $FF_CODEC_INTERFACE ff_mpeg4_nvmpi_decoder;" "$BKP_FILE_LIBAVCODEC_ALLCODECSC"
	if cmp "$BKP_FILE_LIBAVCODEC_ALLCODECSC" "$BKP_FILE_LIBAVCODEC_ALLCODECSC.1"; then return 1; fi;
fi

#add nvmpi vp8 encoder and decoder
if ! grep -q 'ff_vp8_nvmpi_decoder' "$BKP_FILE_LIBAVCODEC_ALLCODECSC"; then
	cp "$BKP_FILE_LIBAVCODEC_ALLCODECSC" "$BKP_FILE_LIBAVCODEC_ALLCODECSC.1"
	sed -i "/$FF_CODEC_INTERFACE ff_vp8_decoder;/a $FF_CODEC_INTERFACE ff_vp8_nvmpi_decoder;" "$BKP_FILE_LIBAVCODEC_ALLCODECSC"
	if cmp "$BKP_FILE_LIBAVCODEC_ALLCODECSC" "$BKP_FILE_LIBAVCODEC_ALLCODECSC.1"; then return 1; fi;
fi

#add nvmpi vp9 encoder and decoder
if ! grep -q 'ff_vp9_nvmpi_decoder' "$BKP_FILE_LIBAVCODEC_ALLCODECSC"; then
	cp "$BKP_FILE_LIBAVCODEC_ALLCODECSC" "$BKP_FILE_LIBAVCODEC_ALLCODECSC.1"
	sed -i "/$FF_CODEC_INTERFACE ff_vp9_decoder;/a $FF_CODEC_INTERFACE ff_vp9_nvmpi_decoder;" "$BKP_FILE_LIBAVCODEC_ALLCODECSC"
	if cmp "$BKP_FILE_LIBAVCODEC_ALLCODECSC" "$BKP_FILE_LIBAVCODEC_ALLCODECSC.1"; then return 1; fi;
fi

#add nvmpi mjpeg decoder
if ! grep -q 'ff_mjpeg_nvmpi_decoder' "$BKP_FILE_LIBAVCODEC_ALLCODECSC"; then
	cp "$BKP_FILE_LIBAVCODEC_ALLCODECSC" "$BKP_FILE_LIBAVCODEC_ALLCODECSC.1"
	sed -i "/$FF_CODEC_INTERFACE ff_mjpeg_decoder;/a $FF_CODEC_INTERFACE ff_mjpeg_nvmpi_decoder;" "$BKP_FILE_LIBAVCODEC_ALLCODECSC"
	if cmp "$BKP_FILE_LIBAVCODEC_ALLCODECSC" "$BKP_FILE_LIBAVCODEC_ALLCODECSC.1"; then return 1; fi;
fi

#add nvmpi mjpeg encoder
if ! grep -q 'ff_mjpeg_nvmpi_encoder' "$BKP_FILE_LIBAVCODEC_ALLCODECSC"; then
	cp "$BKP_FILE_LIBAVCODEC_ALLCODECSC" "$BKP_FILE_LIBAVCODEC_ALLCODECSC.1"
	sed -i "/$FF_CODEC_INTERFACE ff_mjpeg_nvmpi_decoder;/a $FF_CODEC_INTERFACE ff_mjpeg_nvmpi_encoder;" "$BKP_FILE_LIBAVCODEC_ALLCODECSC"
	if cmp "$BKP_FILE_LIBAVCODEC_ALLCODECSC" "$BKP_FILE_LIBAVCODEC_ALLCODECSC.1"; then return 1; fi;
fi

return 0;
}
################## MODIFY libavcodec/allcodecs.c ############################

################## MODIFY libavutil (hwcontext_nvmpi) ############################
function patch_ff_libavutil ()
{
#add AV_HWDEVICE_TYPE_NVMPI to AVHWDeviceType enum in hwcontext.h.
#insert before the closing }; of the enum, anchored from AV_HWDEVICE_TYPE_VULKAN
#(present in all supported FFmpeg versions 6.0+). Works regardless of how many
#entries exist between VULKAN and }; (D3D12VA, AMF, OHCODEC in 8.0+).
if ! grep -q 'AV_HWDEVICE_TYPE_NVMPI' "$BKP_FILE_LIBAVUTIL_HWCONTEXTH"; then
	cp "$BKP_FILE_LIBAVUTIL_HWCONTEXTH" "$BKP_FILE_LIBAVUTIL_HWCONTEXTH.1"
	sed -i '/AV_HWDEVICE_TYPE_VULKAN/,/};/{/};/i\    AV_HWDEVICE_TYPE_NVMPI,
}' "$BKP_FILE_LIBAVUTIL_HWCONTEXTH"
	if cmp "$BKP_FILE_LIBAVUTIL_HWCONTEXTH" "$BKP_FILE_LIBAVUTIL_HWCONTEXTH.1"; then return 1; fi;
fi

#add &ff_hwcontext_type_nvmpi to hw_table[] in hwcontext.c.
#insert before the NULL sentinel, anchored from ff_hwcontext_type_vulkan.
if ! grep -q 'ff_hwcontext_type_nvmpi' "$BKP_FILE_LIBAVUTIL_HWCONTEXTC"; then
	cp "$BKP_FILE_LIBAVUTIL_HWCONTEXTC" "$BKP_FILE_LIBAVUTIL_HWCONTEXTC.1"
	sed -i '/ff_hwcontext_type_vulkan/,/NULL,/{/NULL,/i\#if CONFIG_NVMPI\
    \&ff_hwcontext_type_nvmpi,\
#endif
}' "$BKP_FILE_LIBAVUTIL_HWCONTEXTC"
	if cmp "$BKP_FILE_LIBAVUTIL_HWCONTEXTC" "$BKP_FILE_LIBAVUTIL_HWCONTEXTC.1"; then return 1; fi;
fi

#add "nvmpi" to hw_type_names[] in hwcontext.c.
#insert before the closing }; of the array definition. Anchor on empty brackets
#hw_type_names[] to avoid matching usage sites like hw_type_names[type].
if ! grep -q 'AV_HWDEVICE_TYPE_NVMPI.*nvmpi' "$BKP_FILE_LIBAVUTIL_HWCONTEXTC"; then
	cp "$BKP_FILE_LIBAVUTIL_HWCONTEXTC" "$BKP_FILE_LIBAVUTIL_HWCONTEXTC.1"
	sed -i '/hw_type_names\[\]/,/};/{/};/i\    [AV_HWDEVICE_TYPE_NVMPI]    = "nvmpi",
}' "$BKP_FILE_LIBAVUTIL_HWCONTEXTC"
	if cmp "$BKP_FILE_LIBAVUTIL_HWCONTEXTC" "$BKP_FILE_LIBAVUTIL_HWCONTEXTC.1"; then return 1; fi;
fi

#add extern declaration for ff_hwcontext_type_nvmpi in hwcontext_internal.h.
#insert after ff_hwcontext_type_vulkan (present in all supported versions).
if ! grep -q 'ff_hwcontext_type_nvmpi' "$BKP_FILE_LIBAVUTIL_HWCONTEXT_INTERNALH"; then
	cp "$BKP_FILE_LIBAVUTIL_HWCONTEXT_INTERNALH" "$BKP_FILE_LIBAVUTIL_HWCONTEXT_INTERNALH.1"
	sed -i '/ff_hwcontext_type_vulkan/a extern const HWContextType ff_hwcontext_type_nvmpi;' "$BKP_FILE_LIBAVUTIL_HWCONTEXT_INTERNALH"
	if cmp "$BKP_FILE_LIBAVUTIL_HWCONTEXT_INTERNALH" "$BKP_FILE_LIBAVUTIL_HWCONTEXT_INTERNALH.1"; then return 1; fi;
fi

#add hwcontext_nvmpi.h to HEADERS in libavutil/Makefile.
#insert after hwcontext_drm.h line.
if ! grep -q 'hwcontext_nvmpi.h' "$BKP_FILE_LIBAVUTIL_MAKEFILE"; then
	cp "$BKP_FILE_LIBAVUTIL_MAKEFILE" "$BKP_FILE_LIBAVUTIL_MAKEFILE.1"
	sed -i '/hwcontext_drm.h/a\                                 hwcontext_nvmpi.h                          \\' "$BKP_FILE_LIBAVUTIL_MAKEFILE"
	if cmp "$BKP_FILE_LIBAVUTIL_MAKEFILE" "$BKP_FILE_LIBAVUTIL_MAKEFILE.1"; then return 1; fi;
fi

#add hwcontext_nvmpi.o to OBJS in libavutil/Makefile.
#insert after CONFIG_LIBDRM hwcontext_drm.o line.
if ! grep -q 'hwcontext_nvmpi.o' "$BKP_FILE_LIBAVUTIL_MAKEFILE"; then
	cp "$BKP_FILE_LIBAVUTIL_MAKEFILE" "$BKP_FILE_LIBAVUTIL_MAKEFILE.1"
	sed -i '/OBJS-\$(CONFIG_LIBDRM).*hwcontext_drm.o/a OBJS-$(CONFIG_NVMPI)                    += hwcontext_nvmpi.o' "$BKP_FILE_LIBAVUTIL_MAKEFILE"
	if cmp "$BKP_FILE_LIBAVUTIL_MAKEFILE" "$BKP_FILE_LIBAVUTIL_MAKEFILE.1"; then return 1; fi;
fi

return 0;
}
################## MODIFY libavutil (hwcontext_nvmpi) ############################

################## MODIFY libavfilter (scale_vic filter) #########################
function patch_ff_libavfilter ()
{
#add scale_vic_filter_deps="nvmpi" to configure, after scale_vaapi_filter_deps.
if ! grep -q 'scale_vic_filter_deps="nvmpi"' "$BKP_FILE_CONFIGURE"; then
	cp "$BKP_FILE_CONFIGURE" "$BKP_FILE_CONFIGURE.1"
	sed -i '/scale_vaapi_filter_deps="vaapi"/i scale_vic_filter_deps="nvmpi"' "$BKP_FILE_CONFIGURE"
	if cmp "$BKP_FILE_CONFIGURE" "$BKP_FILE_CONFIGURE.1"; then return 1; fi;
fi

#add extern declaration for ff_vf_scale_vic in allfilters.c.
#FFmpeg 8.0+ (libavfilter >= 11) uses FFFilter; earlier versions use AVFilter.
#detect the correct type from the existing scale_vaapi declaration.
if ! grep -q 'ff_vf_scale_vic' "$BKP_FILE_LIBAVFILTER_ALLFILTERSC"; then
	cp "$BKP_FILE_LIBAVFILTER_ALLFILTERSC" "$BKP_FILE_LIBAVFILTER_ALLFILTERSC.1"
	if grep -q 'FFFilter.*ff_vf_scale_vaapi' "$BKP_FILE_LIBAVFILTER_ALLFILTERSC"; then
		sed -i '/ff_vf_scale_vaapi/a extern const FFFilter ff_vf_scale_vic;' "$BKP_FILE_LIBAVFILTER_ALLFILTERSC"
	else
		sed -i '/ff_vf_scale_vaapi/a extern const AVFilter ff_vf_scale_vic;' "$BKP_FILE_LIBAVFILTER_ALLFILTERSC"
	fi
	if cmp "$BKP_FILE_LIBAVFILTER_ALLFILTERSC" "$BKP_FILE_LIBAVFILTER_ALLFILTERSC.1"; then return 1; fi;
fi

#add OBJS lines for vf_scale_vic.o and vf_scale_vic_frame.o in libavfilter/Makefile.
#insert after SCALE_VAAPI_FILTER line. Split into setup (vf_scale_vic.c) and
#per-frame processing (vf_scale_vic_frame.c) translation units — see issue #105.
if ! grep -q 'SCALE_VIC_FILTER' "$BKP_FILE_LIBAVFILTER_MAKEFILE"; then
	cp "$BKP_FILE_LIBAVFILTER_MAKEFILE" "$BKP_FILE_LIBAVFILTER_MAKEFILE.1"
	sed -i '/OBJS-\$(CONFIG_SCALE_VAAPI_FILTER)/a OBJS-$(CONFIG_SCALE_VIC_FILTER)              += vf_scale_vic.o' "$BKP_FILE_LIBAVFILTER_MAKEFILE"
	sed -i '/OBJS-\$(CONFIG_SCALE_VIC_FILTER).*vf_scale_vic\.o/a OBJS-$(CONFIG_SCALE_VIC_FILTER)              += vf_scale_vic_frame.o' "$BKP_FILE_LIBAVFILTER_MAKEFILE"
	if cmp "$BKP_FILE_LIBAVFILTER_MAKEFILE" "$BKP_FILE_LIBAVFILTER_MAKEFILE.1"; then return 1; fi;
fi

return 0;
}
################## MODIFY libavfilter (scale_vic filter) #########################

if path_ff_configure 2>&1 > /dev/null; then echo "$FF_FILE_CONFIGURE is successfully patched!"; else echo "Patching $FF_FILE_CONFIGURE failed!"; exit 1; fi;
if path_ff_libavcodec_Makefile 2>&1 > /dev/null; then echo "$FF_FILE_LIBAVCODEC_MAKEFILE is successfully patched!"; else echo "Patching $FF_FILE_LIBAVCODEC_MAKEFILE failed!"; exit 1; fi;
if path_ff_libavcodec_allcodecsc 2>&1 > /dev/null; then echo "$FF_FILE_LIBAVCODEC_ALLCODECSC is successfully patched!"; else echo "Patching $FF_FILE_LIBAVCODEC_ALLCODECSC failed!"; exit 1; fi;
if patch_ff_libavutil 2>&1 > /dev/null; then echo "libavutil hwcontext_nvmpi is successfully patched!"; else echo "Patching libavutil failed!"; exit 1; fi;
if patch_ff_libavfilter 2>&1 > /dev/null; then echo "libavfilter scale_vic is successfully patched!"; else echo "Patching libavfilter failed!"; exit 1; fi;

cp "$BKP_FILE_CONFIGURE" "$FF_FILE_CONFIGURE"
cp "$BKP_FILE_LIBAVCODEC_MAKEFILE" "$FF_FILE_LIBAVCODEC_MAKEFILE"
cp "$BKP_FILE_LIBAVCODEC_ALLCODECSC" "$FF_FILE_LIBAVCODEC_ALLCODECSC"
cp "$BKP_FILE_LIBAVUTIL_MAKEFILE" "$FF_FILE_LIBAVUTIL_MAKEFILE"
cp "$BKP_FILE_LIBAVUTIL_HWCONTEXTH" "$FF_FILE_LIBAVUTIL_HWCONTEXTH"
cp "$BKP_FILE_LIBAVUTIL_HWCONTEXTC" "$FF_FILE_LIBAVUTIL_HWCONTEXTC"
cp "$BKP_FILE_LIBAVUTIL_HWCONTEXT_INTERNALH" "$FF_FILE_LIBAVUTIL_HWCONTEXT_INTERNALH"
cp "$BKP_FILE_LIBAVFILTER_MAKEFILE" "$FF_FILE_LIBAVFILTER_MAKEFILE"
cp "$BKP_FILE_LIBAVFILTER_ALLFILTERSC" "$FF_FILE_LIBAVFILTER_ALLFILTERSC"

#copy nvmpi enc, dec, and dynlink files to ffmpeg libavcodec dir
cp "${REPO_ROOT}/ffmpeg/dev/common/libavcodec/dynlink_nvmpi.h" ${FF_DIR_LIBAVCODEC}"/dynlink_nvmpi.h"
cp "${REPO_ROOT}/ffmpeg/dev/common/libavcodec/nvmpi_dec.c" ${FF_DIR_LIBAVCODEC}"/nvmpi_dec.c"
cp "${REPO_ROOT}/ffmpeg/dev/common/libavcodec/nvmpi_dec_mjpeg.c" ${FF_DIR_LIBAVCODEC}"/nvmpi_dec_mjpeg.c"
cp "${REPO_ROOT}/ffmpeg/dev/common/libavcodec/nvmpi_enc.c" ${FF_DIR_LIBAVCODEC}"/nvmpi_enc.c"
cp "${REPO_ROOT}/ffmpeg/dev/common/libavcodec/nvmpi_enc_runtime.c" ${FF_DIR_LIBAVCODEC}"/nvmpi_enc_runtime.c"
cp "${REPO_ROOT}/ffmpeg/dev/common/libavcodec/nvmpi_enc_ff_internal.h" ${FF_DIR_LIBAVCODEC}"/nvmpi_enc_ff_internal.h"
cp "${REPO_ROOT}/ffmpeg/dev/common/libavcodec/nvmpi_enc_jpeg.c" ${FF_DIR_LIBAVCODEC}"/nvmpi_enc_jpeg.c"

#copy hwcontext_nvmpi files to ffmpeg libavutil dir
cp "${REPO_ROOT}/ffmpeg/dev/common/libavutil/hwcontext_nvmpi.h" ${FF_DIR_LIBAVUTIL}"/hwcontext_nvmpi.h"
cp "${REPO_ROOT}/ffmpeg/dev/common/libavutil/hwcontext_nvmpi.c" ${FF_DIR_LIBAVUTIL}"/hwcontext_nvmpi.c"
cp "${REPO_ROOT}/ffmpeg/dev/common/libavutil/dynlink_nvmpi_cuda.h" ${FF_DIR_LIBAVUTIL}"/dynlink_nvmpi_cuda.h"

#copy scale_vic filter files to ffmpeg libavfilter dir
cp "${REPO_ROOT}/ffmpeg/dev/common/libavfilter/vf_scale_vic.c" ${FF_DIR_LIBAVFILTER}"/vf_scale_vic.c"
cp "${REPO_ROOT}/ffmpeg/dev/common/libavfilter/vf_scale_vic_frame.c" ${FF_DIR_LIBAVFILTER}"/vf_scale_vic_frame.c"
cp "${REPO_ROOT}/ffmpeg/dev/common/libavfilter/vf_scale_vic_internal.h" ${FF_DIR_LIBAVFILTER}"/vf_scale_vic_internal.h"
cp "${REPO_ROOT}/ffmpeg/dev/common/libavfilter/dynlink_nvmpi_vic.h" ${FF_DIR_LIBAVFILTER}"/dynlink_nvmpi_vic.h"

echo "Success!"

rm -rf "$BKP_DIR" 2>&1 > /dev/null

exit 0
