cmake_minimum_required(VERSION 3.9)
project(nvmpi VERSION 2.0.0 DESCRIPTION "nvidia multimedia api")

set(JETSON_MULTIMEDIA_API_DIR_DEFAULT "/usr/src/jetson_multimedia_api")
set(JETSON_MULTIMEDIA_LIB_DIR_DEFAULT "/usr/lib/aarch64-linux-gnu/tegra")
set(CUDA_INCLUDE_DIR_DEFAULT "/usr/local/cuda/include")
set(CUDA_LIB_DIR_DEFAULT "/usr/local/cuda/lib64")

option(WITH_STUBS "Build nvmpi library and link stubs instead of original libraries. Default: OFF." OFF)
option(JETSON_MULTIMEDIA_API_DIR "Path to Jetson Multimedia API headers and common sources directory. Default: ${JETSON_MULTIMEDIA_API_DIR_DEFAULT}.")
option(JETSON_MULTIMEDIA_LIB_DIR "Path to Jetson Multimedia libraries directory. Default: ${JETSON_MULTIMEDIA_LIB_DIR_DEFAULT}.")
option(CUDA_INCLUDE_DIR "Path to CUDA headers directory. Default: ${CUDA_INCLUDE_DIR_DEFAULT}.")
option(CUDA_LIB_DIR "Path to CUDA libraries directory. Default: ${CUDA_LIB_DIR_DEFAULT}.")

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED on)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(NOT JETSON_MULTIMEDIA_API_DIR)
	set(JETSON_MULTIMEDIA_API_DIR "${JETSON_MULTIMEDIA_API_DIR_DEFAULT}")
endif()

if(WITH_STUBS AND NOT JETSON_MULTIMEDIA_LIB_DIR)
	set(JETSON_MULTIMEDIA_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/stubs")
endif()

if(NOT JETSON_MULTIMEDIA_LIB_DIR)
	set(JETSON_MULTIMEDIA_LIB_DIR "${JETSON_MULTIMEDIA_LIB_DIR_DEFAULT}")
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

set(NVMPI_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
set(NVMPI_PUBLIC_HEADER "${NVMPI_INCLUDE_DIR}/nvmpi.h")
set(JETSON_MULTIMEDIA_API_INCLUDE_DIR "${JETSON_MULTIMEDIA_API_DIR}/include")
set(JETSON_MULTIMEDIA_API_SRC_DIR "${JETSON_MULTIMEDIA_API_DIR}/samples/common/classes")

set(NVMPI_INCLUDE_DIRS
	${NVMPI_INCLUDE_DIR}
	${CUDA_INCLUDE_DIR}
	${JETSON_MULTIMEDIA_API_INCLUDE_DIR})

set(JETSON_MULTIMEDIA_API_SRC
    ${JETSON_MULTIMEDIA_API_SRC_DIR}/NvBuffer.cpp
    ${JETSON_MULTIMEDIA_API_SRC_DIR}/NvElement.cpp
    ${JETSON_MULTIMEDIA_API_SRC_DIR}/NvElementProfiler.cpp
    ${JETSON_MULTIMEDIA_API_SRC_DIR}/NvLogging.cpp
    ${JETSON_MULTIMEDIA_API_SRC_DIR}/NvV4l2Element.cpp
    ${JETSON_MULTIMEDIA_API_SRC_DIR}/NvV4l2ElementPlane.cpp
    ${JETSON_MULTIMEDIA_API_SRC_DIR}/NvVideoDecoder.cpp
    ${JETSON_MULTIMEDIA_API_SRC_DIR}/NvVideoEncoder.cpp)

set(NVMPI_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nvmpi_dec.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nvmpi_enc.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/NVMPI_frameBuf.cpp)
    
set(NVMPI_DEP_LIBS ${CMAKE_THREAD_LIBS_INIT} ${LIB_V4L2} ${LIB_NVJPEG})

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
configure_file(nvmpi.pc.in nvmpi.pc @ONLY)
include(GNUInstallDirs)
install(TARGETS nvmpi nvmpi_static
	    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
	    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
	        PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES ${CMAKE_BINARY_DIR}/nvmpi.pc
	    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/pkgconfig)
install(FILES ${CMAKE_BINARY_DIR}/nvmpi.pc
	    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
