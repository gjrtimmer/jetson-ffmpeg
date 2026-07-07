cmake_minimum_required(VERSION 3.10)
project(nvmpi VERSION 3.10.0 DESCRIPTION "nvidia multimedia api")

# ---------------------------------------------------------------------------
# Cross-compile sysroot
# ---------------------------------------------------------------------------
# Prefix for all default Jetson/CUDA paths. Empty on native builds.
# Precedence: explicit -DSYSROOT > env BITBAKE_SYSROOT (Yocto) > env TARGET_ROOTFS.
# NOT inherited from CMAKE_SYSROOT — when a toolchain file sets CMAKE_SYSROOT,
# CMake auto-prepends it to find_library/find_file PATHS; inheriting would
# cause double-prefixing (sysroot/sysroot/usr/lib/...). Users with toolchain
# files should pass explicit -DJETSON_MULTIMEDIA_API_DIR=... instead.
set(SYSROOT "" CACHE PATH "Cross-compile sysroot prefix for Jetson libraries and headers.")
if(NOT SYSROOT)
	if(DEFINED ENV{BITBAKE_SYSROOT})
		set(SYSROOT "$ENV{BITBAKE_SYSROOT}")
	elseif(DEFINED ENV{TARGET_ROOTFS})
		set(SYSROOT "$ENV{TARGET_ROOTFS}")
	endif()
endif()
if(SYSROOT)
	message(STATUS "Cross-compile sysroot: ${SYSROOT}")
endif()

# ---------------------------------------------------------------------------
# Path defaults (sysroot-prefixed when cross-compiling)
# ---------------------------------------------------------------------------
set(JETSON_MULTIMEDIA_API_DIR_DEFAULT "${SYSROOT}/usr/src/jetson_multimedia_api")
# JP6 moved tegra libs from tegra/ to nvidia/
set(JETSON_MULTIMEDIA_LIB_DIR_JP5 "${SYSROOT}/usr/lib/aarch64-linux-gnu/tegra")
set(JETSON_MULTIMEDIA_LIB_DIR_JP6 "${SYSROOT}/usr/lib/aarch64-linux-gnu/nvidia")
set(CUDA_INCLUDE_DIR_DEFAULT "${SYSROOT}/usr/local/cuda/include")
set(CUDA_LIB_DIR_DEFAULT "${SYSROOT}/usr/local/cuda/lib64")

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(WITH_STUBS "Build against stubs instead of real Jetson libraries." OFF)
option(WITH_VENDORED_NVV4L2 "Use vendored NvV4l2ElementPlane.cpp with pthread_join guard." ON)

# Path options — CACHE PATH, not option() (option() is boolean-only)
set(JETSON_MULTIMEDIA_API_DIR "" CACHE PATH
	"Jetson Multimedia API headers and sources directory.")
set(JETSON_MULTIMEDIA_LIB_DIR "" CACHE PATH
	"Jetson Multimedia libraries directory.")
set(CUDA_INCLUDE_DIR "" CACHE PATH "CUDA headers directory.")
set(CUDA_LIB_DIR "" CACHE PATH "CUDA libraries directory.")

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED on)

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()

# ---------------------------------------------------------------------------
# Resolve paths: explicit -D > probed JP6/JP5 > default
# ---------------------------------------------------------------------------
if(NOT JETSON_MULTIMEDIA_API_DIR)
	set(JETSON_MULTIMEDIA_API_DIR "${JETSON_MULTIMEDIA_API_DIR_DEFAULT}")
endif()

if(WITH_STUBS AND NOT JETSON_MULTIMEDIA_LIB_DIR)
	set(JETSON_MULTIMEDIA_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/stubs")
elseif(NOT JETSON_MULTIMEDIA_LIB_DIR)
	if(EXISTS "${JETSON_MULTIMEDIA_LIB_DIR_JP6}")
		set(JETSON_MULTIMEDIA_LIB_DIR "${JETSON_MULTIMEDIA_LIB_DIR_JP6}")
		message(STATUS "JetPack 6 detected (nvidia lib dir)")
	elseif(EXISTS "${JETSON_MULTIMEDIA_LIB_DIR_JP5}")
		set(JETSON_MULTIMEDIA_LIB_DIR "${JETSON_MULTIMEDIA_LIB_DIR_JP5}")
		message(STATUS "JetPack 4/5 detected (tegra lib dir)")
	else()
		set(JETSON_MULTIMEDIA_LIB_DIR "${JETSON_MULTIMEDIA_LIB_DIR_JP5}")
	endif()
endif()

if(NOT CUDA_INCLUDE_DIR)
	set(CUDA_INCLUDE_DIR "${CUDA_INCLUDE_DIR_DEFAULT}")
endif()

if(NOT CUDA_LIB_DIR)
	set(CUDA_LIB_DIR "${CUDA_LIB_DIR_DEFAULT}")
endif()

set(OPT_FLAGS "-Ofast -march=native")
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${OPT_FLAGS}")
#set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${OPT_FLAGS}")

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wno-unused-value -fPIC")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wno-unused-value -fPIC")

set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,-rpath-link=${JETSON_MULTIMEDIA_LIB_DIR}")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,-rpath-link=${CUDA_LIB_DIR}")

find_library(LIB_V4L2 nvv4l2 PATHS "${JETSON_MULTIMEDIA_LIB_DIR}")
find_library(LIB_NVJPEG nvjpeg PATHS "${JETSON_MULTIMEDIA_LIB_DIR}")
find_package (Threads)

# NvJpegDecoder.cpp calls standard libjpeg functions (jpeg_read_raw_data etc.).
# Jetson ships libjpeg.so.8 but not the -dev symlink; find_library needs the
# unversioned name or NAMES with the soname. Stubs fallback: link -ljpeg
# against the stub libjpeg.so we ship.
find_library(LIB_JPEG NAMES jpeg PATHS "${JETSON_MULTIMEDIA_LIB_DIR}" "/usr/lib/aarch64-linux-gnu")
if(NOT LIB_JPEG)
	find_file(LIB_JPEG_SONAME "libjpeg.so.8" PATHS "/usr/lib/aarch64-linux-gnu" "/usr/lib")
	if(LIB_JPEG_SONAME)
		set(LIB_JPEG "${LIB_JPEG_SONAME}")
	else()
		set(LIB_JPEG "jpeg")
		message(STATUS "libjpeg not found — will link -ljpeg (stubs or system).")
	endif()
endif()

set(NVMPI_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
set(NVMPI_PUBLIC_HEADER "${NVMPI_INCLUDE_DIR}/nvmpi.h")
set(JETSON_MULTIMEDIA_API_INCLUDE_DIR "${JETSON_MULTIMEDIA_API_DIR}/include")
set(JETSON_MULTIMEDIA_API_SRC_DIR "${JETSON_MULTIMEDIA_API_DIR}/samples/common/classes")

set(NVMPI_INCLUDE_DIRS
	${NVMPI_INCLUDE_DIR}
	${CUDA_INCLUDE_DIR}
	${JETSON_MULTIMEDIA_API_INCLUDE_DIR}
	${JETSON_MULTIMEDIA_API_INCLUDE_DIR}/libjpeg-8b)

if(WITH_VENDORED_NVV4L2)
    set(NVV4L2_ELEMENT_PLANE_SRC "${CMAKE_CURRENT_SOURCE_DIR}/vendor/mmapi/NvV4l2ElementPlane.cpp")
    message(STATUS "Using vendored NvV4l2ElementPlane.cpp (pthread_join guard for glibc>=2.34)")
else()
    set(NVV4L2_ELEMENT_PLANE_SRC "${JETSON_MULTIMEDIA_API_SRC_DIR}/NvV4l2ElementPlane.cpp")
    message(STATUS "Using system NvV4l2ElementPlane.cpp")
endif()

set(JETSON_MULTIMEDIA_API_SRC
    ${JETSON_MULTIMEDIA_API_SRC_DIR}/NvBuffer.cpp
    ${JETSON_MULTIMEDIA_API_SRC_DIR}/NvElement.cpp
    ${JETSON_MULTIMEDIA_API_SRC_DIR}/NvElementProfiler.cpp
    ${JETSON_MULTIMEDIA_API_SRC_DIR}/NvLogging.cpp
    ${JETSON_MULTIMEDIA_API_SRC_DIR}/NvV4l2Element.cpp
    ${NVV4L2_ELEMENT_PLANE_SRC}
    ${JETSON_MULTIMEDIA_API_SRC_DIR}/NvVideoDecoder.cpp
    ${JETSON_MULTIMEDIA_API_SRC_DIR}/NvVideoEncoder.cpp
    ${JETSON_MULTIMEDIA_API_SRC_DIR}/NvJpegDecoder.cpp
    ${JETSON_MULTIMEDIA_API_SRC_DIR}/NvJpegEncoder.cpp)

set(NVMPI_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nvmpi_dec_init.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nvmpi_dec_api.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nvmpi_dec_capture.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nvmpi_dec_planes.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nvmpi_enc_init.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nvmpi_enc_input.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nvmpi_enc_api.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nvmpi_enc_output.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nvmpi_frame_buffer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nvmpi_dec_jpeg.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nvmpi_dec_jpeg_ctx.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nvmpi_enc_jpeg.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nvmpi_surface.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nvmpi_vic.cpp)
    
set(NVMPI_DEP_LIBS ${CMAKE_THREAD_LIBS_INIT} ${LIB_V4L2} ${LIB_NVJPEG} ${LIB_JPEG})

#if NvUtils API is present prefer it to nvbuf_utils 
if(EXISTS "${JETSON_MULTIMEDIA_API_INCLUDE_DIR}/nvbufsurface.h")
	add_definitions(-DWITH_NVUTILS)
	find_library(LIB_NVBUFSURFACE nvbufsurface PATHS "${JETSON_MULTIMEDIA_LIB_DIR}")
	find_library(LIB_NVBUFSURFTRANSFORM nvbufsurftransform PATHS "${JETSON_MULTIMEDIA_LIB_DIR}")
	set(JETSON_MULTIMEDIA_API_SRC ${JETSON_MULTIMEDIA_API_SRC}
		${JETSON_MULTIMEDIA_API_SRC_DIR}/NvBufSurface.cpp)
	set(NVMPI_DEP_LIBS ${NVMPI_DEP_LIBS} ${LIB_NVBUFSURFACE} ${LIB_NVBUFSURFTRANSFORM})
	message(STATUS "Using NvUtils API.")
else ()
	find_library(LIB_NVBUF nvbuf_utils PATHS "${JETSON_MULTIMEDIA_LIB_DIR}")
	set(NVMPI_DEP_LIBS ${NVMPI_DEP_LIBS} ${LIB_NVBUF})
	message(STATUS "Using NvBufUtils API.")
endif()

set(NVMPI_SRC ${NVMPI_SRC} ${JETSON_MULTIMEDIA_API_SRC})

# Tegra's custom libjpeg.so.8 exports jpeg_set_hardware_acceleration_parameters_{enc,dec}
# which NvJpegEncoder.cpp / NvJpegDecoder.cpp call. System libjpeg lacks these symbols;
# in stubs mode, compile no-op implementations so the linker is satisfied.
if(WITH_STUBS)
	set(NVMPI_SRC ${NVMPI_SRC} ${CMAKE_CURRENT_SOURCE_DIR}/stubs/jpeg_tegra_stubs.c)
endif()

#NVIDIA's sample classes are external (host-provided) and not -Wextra clean:
#they trigger benign -Wmissing-field-initializers/-Wunused-parameter noise.
#Suppress those two warnings for these files only; src/ keeps full -Wall -Wextra.
set_source_files_properties(${JETSON_MULTIMEDIA_API_SRC} PROPERTIES
	COMPILE_OPTIONS "-Wno-missing-field-initializers;-Wno-unused-parameter")

add_library(nvmpi SHARED ${NVMPI_SRC})
add_library(nvmpi_static STATIC ${NVMPI_SRC})

set_target_properties(nvmpi_static PROPERTIES OUTPUT_NAME nvmpi)
set_target_properties(nvmpi PROPERTIES VERSION ${PROJECT_VERSION})
set_target_properties(nvmpi PROPERTIES SOVERSION 1)
set_target_properties(nvmpi nvmpi_static PROPERTIES PUBLIC_HEADER ${NVMPI_PUBLIC_HEADER})
set_target_properties(nvmpi PROPERTIES LINK_FLAGS "-Wl,--no-as-needed")
target_link_libraries(nvmpi PRIVATE ${NVMPI_DEP_LIBS})
target_include_directories(nvmpi PRIVATE ${NVMPI_INCLUDE_DIRS})
target_include_directories(nvmpi_static PRIVATE ${NVMPI_INCLUDE_DIRS})
include(GNUInstallDirs)
configure_file(nvmpi.pc.in nvmpi.pc @ONLY)
install(TARGETS nvmpi nvmpi_static
	    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
	    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
	        PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES ${CMAKE_BINARY_DIR}/nvmpi.pc
	    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/pkgconfig)
install(FILES ${CMAKE_BINARY_DIR}/nvmpi.pc
	    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

find_package(Threads REQUIRED)
include(CTest)
if(BUILD_TESTING)
  add_executable(test_bufpool test/unit/test_bufpool.cpp)
  target_include_directories(test_bufpool PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(test_bufpool PRIVATE Threads::Threads)
  add_test(NAME bufpool COMMAND test_bufpool)

  add_executable(test_api test/api/test_api.c)
  target_include_directories(test_api PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(test_api PRIVATE nvmpi Threads::Threads)
  add_test(NAME api COMMAND test_api)
endif()

# ---------------------------------------------------------------------------
# CPack .deb packaging — `cmake --build . --target package`
# ---------------------------------------------------------------------------
set(CPACK_PACKAGE_NAME "libnvmpi")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_CONTACT "https://github.com/gjrtimmer/jetson-ffmpeg")
set(CPACK_GENERATOR "DEB")
set(CPACK_DEBIAN_PACKAGE_SECTION "libs")
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "arm64")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
include(CPack)
