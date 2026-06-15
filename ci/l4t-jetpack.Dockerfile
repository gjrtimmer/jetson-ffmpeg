# Thin wrapper around NVIDIA's l4t-jetpack image.
#
# Adds non-interactive dpkg defaults so apt-get never prompts for conffile
# conflicts in CI (the NVIDIA BSP packages ship config files that already
# exist in the base image, triggering interactive dpkg prompts that fail
# in non-interactive CI jobs).
#
# Rebuild when upgrading L4T/JetPack.
ARG L4T_TAG=r36.4.0
FROM nvcr.io/nvidia/l4t-jetpack:${L4T_TAG}

ARG DEBIAN_FRONTEND=noninteractive

RUN echo 'Dpkg::Options { "--force-confdef"; "--force-confold"; };' \
        > /etc/apt/apt.conf.d/99-force-confold

# jetson-stats: provides jtop Python API for NVDEC/NVENC/GPU utilization
# metrics in hw-perf-bench.sh during CI hw-test jobs.
RUN pip3 install --no-cache-dir jetson-stats
