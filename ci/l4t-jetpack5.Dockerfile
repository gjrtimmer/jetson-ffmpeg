# Thin wrapper around NVIDIA's l4t-jetpack image for JetPack 5.x (Xavier/Orin, kernel 5.10).
#
# JetPack 5.x is the terminal release for Xavier hardware (AGX Xavier, Xavier NX).
# All JP5 releases (5.0.2-5.1.6) share kernel 5.10 V4L2 headers, so a single
# image covers the entire family.
#
# Uses l4t-jetpack:r35.4.1 (JP5.1.3) — the last published JP5 NGC image.
# Mirrors the JP6 l4t-jetpack6 image pattern.
#
# Adds non-interactive dpkg defaults so apt-get never prompts for conffile
# conflicts in CI.
#
# Rebuild when upgrading the JP5 L4T baseline.
ARG L4T_TAG=r35.4.1
FROM nvcr.io/nvidia/l4t-jetpack:${L4T_TAG}

ARG DEBIAN_FRONTEND=noninteractive

RUN echo 'Dpkg::Options { "--force-confdef"; "--force-confold"; };' \
        > /etc/apt/apt.conf.d/99-force-confold
