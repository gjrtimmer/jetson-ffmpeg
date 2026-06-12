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

Once installed, any FFmpeg-based application can offload H.264, HEVC, MPEG-2,
MPEG-4, VP8, and VP9 decoding — and H.264/HEVC encoding — to the Jetson's
hardware engines, leaving the CPU free for your actual workload:

```bash
ffmpeg -c:v h264_nvmpi -i input.mp4 -c:v hevc_nvmpi -b:v 4M output.mp4
```

## Compatibility

jetson-ffmpeg runs on Jetson platforms from the original Nano through the
Orin family (JetPack 4.x – 6.x) and supports FFmpeg **4.2 up to 8.0+**.

See **[docs/COMPATIBILITY.md](docs/COMPATIBILITY.md)** for the supported
codecs, the full Jetson/JetPack support matrix, and the list of tested
FFmpeg releases.

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
examples see **[docs/BUILD.md](docs/BUILD.md)**.

## Documentation

- **[Build Guide](docs/BUILD.md)** — Complete build, installation, and usage instructions
- **[Compatibility](docs/COMPATIBILITY.md)** — Supported codecs, Jetson/JetPack support matrix, FFmpeg versions
- **[Scripts Reference](docs/SCRIPTS.md)** — Every script, command, and dev-container alias
- **[Development Guide](docs/DEVELOPMENT.md)** — Architecture, patch system, and how to add new FFmpeg versions
