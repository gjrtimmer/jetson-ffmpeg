# jetson-ffmpeg

Hardware-accelerated video encoding and decoding for NVIDIA Jetson, fully
integrated into FFmpeg.

Every NVIDIA Jetson module ships dedicated silicon for video encoding and
decoding, but a stock FFmpeg build cannot use it. **jetson-ffmpeg** bridges
that gap with two components:

- **libnvmpi** — a lightweight shared library that wraps NVIDIA's Jetson
  Multimedia API (V4L2/NvBuffer) behind a simple C interface.
- **FFmpeg patches** — register native `*_nvmpi` decoders and encoders in
  FFmpeg that drive the hardware through libnvmpi.

### Performance

jetson-ffmpeg now enables **hardware max-performance mode** by default, lifting
the NVDEC/NVENC clock governor for maximum throughput:

| Workload | Without `max_perf` | With `max_perf` | Speedup |
|----------|-------------------|-----------------|---------|
| 720p H.264 decode | ~46 fps | ~290 fps | **~6x** |
| 4K HEVC decode | ~28 fps | ~64 fps | **~2.3x** |

Additional low-latency options: `disable_dpb` (decoder) skips picture-buffer
reordering, `poc_type=2` (encoder) removes reorder latency for H.264 streams.

Once installed, any FFmpeg-based application can offload H.264, HEVC, MPEG-2,
MPEG-4, VP8, and VP9 decoding — and H.264/HEVC encoding — to the Jetson's
hardware engines, leaving the CPU free for your actual workload:

```bash
ffmpeg -c:v h264_nvmpi -i input.mp4 -c:v hevc_nvmpi -b:v 4M output.mp4
```

## Compatibility

jetson-ffmpeg targets Jetson platforms from the original Nano (JetPack 4.x)
through the Orin family (JetPack 5.x/6.x) and supports FFmpeg **4.2 up to
8.0+**. Hardware testing currently covers Orin NX on JetPack 6; other
platforms are expected to work but are untested.

See the **[Compatibility](https://github.com/gjrtimmer/jetson-ffmpeg/wiki/Compatibility)**
wiki page for the supported codecs, the full Jetson/JetPack support matrix, and
the list of tested FFmpeg releases.

## Quick Start

```bash
# 1. Build and install libnvmpi
git clone https://github.com/Keylost/jetson-ffmpeg.git
cd jetson-ffmpeg
./scripts/build.sh --install        # or: mkdir build && cd build && cmake .. && make && sudo make install && sudo ldconfig

# 2. Patch and build FFmpeg
git clone git://source.ffmpeg.org/ffmpeg.git -b release/7.1 --depth=1
cd jetson-ffmpeg
./scripts/ffpatch.sh ../ffmpeg
cd ../ffmpeg
./configure --enable-nvmpi
make
sudo make install
```

For full build instructions, CMake options, cross-compilation, and usage
examples see the **[Build and Install](https://github.com/gjrtimmer/jetson-ffmpeg/wiki/Build-and-Install)**
wiki page.

## Documentation

📖 **Full documentation lives in the [project wiki](https://github.com/gjrtimmer/jetson-ffmpeg/wiki).**

- **[Build and Install](https://github.com/gjrtimmer/jetson-ffmpeg/wiki/Build-and-Install)** — Complete build, installation, and usage instructions
- **[Compatibility](https://github.com/gjrtimmer/jetson-ffmpeg/wiki/Compatibility)** — Supported codecs, Jetson/JetPack support matrix, FFmpeg versions
- **[Scripts and Commands](https://github.com/gjrtimmer/jetson-ffmpeg/wiki/Scripts-and-Commands)** — Every script, command, and dev-container alias
- **[FAQ & Known Limitations](https://github.com/gjrtimmer/jetson-ffmpeg/wiki/FAQ)** — HEVC/RTMP, 4K encode cap, Orin Nano NVENC, mod-16, pkg-config, performance
- **[Development Guide](https://github.com/gjrtimmer/jetson-ffmpeg/wiki/Development-Guide)** — Architecture, patch system, and how to add new FFmpeg versions

## Support This Project

If jetson-ffmpeg is useful to you, please consider supporting its development:

[![Buy Me A Coffee](https://img.shields.io/badge/Buy%20Me%20A%20Coffee-support-yellow?style=flat&logo=buy-me-a-coffee)](https://buymeacoffee.com/gjrtimmer)

Maintaining and improving hardware-accelerated video on Jetson takes significant
time and resources. Your support helps keep development going — fixing bugs
faster, adding new features, and keeping up with new FFmpeg releases and Jetson
platforms. Every contribution, no matter the size, is appreciated.
