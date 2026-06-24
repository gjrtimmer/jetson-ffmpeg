# Example CMake toolchain file for cross-compiling libnvmpi from an x86_64
# host targeting aarch64 Jetson. Adjust SYSROOT to point at a directory
# containing the Jetson root filesystem (headers, libraries, MMAPI sources).
#
# Usage:
#   cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64.cmake \
#         -DSYSROOT=/path/to/jetson-rootfs \
#         -B build-cross .

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER   aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

# When using this toolchain, pass -DSYSROOT=/path/to/jetson-rootfs on the
# cmake command line. The CMakeLists.txt SYSROOT variable prefixes all
# Jetson/CUDA default paths so find_library and EXISTS checks resolve
# against the sysroot.
