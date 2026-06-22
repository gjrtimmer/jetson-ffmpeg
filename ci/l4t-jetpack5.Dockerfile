# Base image for JetPack 5.x (Xavier/Orin, kernel 5.10) compile-testing.
#
# JetPack 5.x is the terminal release for Xavier hardware (AGX Xavier, Xavier NX).
# All JP5 releases (5.0.2-5.1.6) share kernel 5.10 V4L2 headers, so a single
# image covers the entire family.
#
# Uses l4t-base (not l4t-jetpack) because l4t-jetpack is ~5GB and exceeds the
# CI job timeout during image build. l4t-base is ~400MB and provides the kernel
# V4L2 headers; the Multimedia API is installed via apt in the builder layer.
#
# Adds non-interactive dpkg defaults so apt-get never prompts for conffile
# conflicts in CI.
#
# Rebuild when upgrading the JP5 L4T baseline.
ARG L4T_TAG=r35.4.1
FROM nvcr.io/nvidia/l4t-base:${L4T_TAG}

ARG DEBIAN_FRONTEND=noninteractive

RUN echo 'Dpkg::Options { "--force-confdef"; "--force-confold"; };' \
        > /etc/apt/apt.conf.d/99-force-confold
