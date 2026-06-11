# jetson-ffmpeg
L4T Multimedia API for ffmpeg.  
This library provides the ability to use hardware acceleration for video encoding and decoding on Nvidia Jetson platforms with the FFmpeg multimedia framework.

### Jetson/JetPack support table
  - :white_check_mark: - Fully supported.
  - :large_blue_circle: - Not tested.
  - :x: - Not supported.
  - :large_orange_diamond: - There is no JetPack version available for this platform.
    
| 			    | TK1 | TX1 | TX2 | TX2i | Nano | AGX Xavier | Xavier NX | AGX Orin | Orin NX | Orin Nano |
| ------------- | --- | --- | --- | ---- | ----	| ---------	 | --------- | -------- | ------- | --------- |
| JetPack 1.0.x | :large_blue_circle: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: |
| JetPack 1.1.x | :large_blue_circle: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: |
| JetPack 1.2.x | :large_blue_circle: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: |
| JetPack 2.0.x | :large_blue_circle: | :large_blue_circle: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: |
| JetPack 2.1.x | :large_blue_circle: | :large_blue_circle: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: |
| JetPack 2.2.x | :large_blue_circle: | :large_blue_circle: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: |
| JetPack 2.3.x | :large_blue_circle: | :large_blue_circle: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: |
| JetPack 3.0.x | :large_blue_circle: | :large_blue_circle: | :large_blue_circle: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: |
| JetPack 3.1.x | :large_blue_circle: | :large_blue_circle: | :large_blue_circle: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: |
| JetPack 3.2.x | :large_orange_diamond: | :large_blue_circle: | :large_blue_circle: | :large_blue_circle: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: |
| JetPack 3.3.x | :large_orange_diamond: | :large_blue_circle: | :large_blue_circle: | :large_blue_circle: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: |
| JetPack 4.1.x | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_blue_circle: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: |
| JetPack 4.2.x | :large_orange_diamond: | :large_blue_circle: | :large_blue_circle: | :large_blue_circle: | :large_blue_circle: | :large_blue_circle: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: |
| JetPack 4.3.x | :large_orange_diamond: | :large_blue_circle: | :large_blue_circle: | :large_blue_circle: | :large_blue_circle: | :large_blue_circle: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: |
| JetPack 4.4.x | :large_orange_diamond: | :large_blue_circle: | :large_blue_circle: | :large_blue_circle: | :large_blue_circle: | :large_blue_circle: | :large_blue_circle: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: |
| JetPack 4.5.x | :large_orange_diamond: | :large_blue_circle: | :large_blue_circle: | :large_blue_circle: | :white_check_mark: | :white_check_mark: | :white_check_mark: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: |
| JetPack 4.6.x | :large_orange_diamond: | :large_blue_circle: | :large_blue_circle: | :large_blue_circle: | :white_check_mark: | :white_check_mark: | :white_check_mark: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: |
| JetPack 5.0.x | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :white_check_mark: | :white_check_mark: | :white_check_mark: | :large_orange_diamond: | :large_orange_diamond: |
| JetPack 5.1.x | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :white_check_mark: | :white_check_mark: | :white_check_mark: | :white_check_mark: | :white_check_mark: |
| JetPack 6.0.x | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :white_check_mark: | :white_check_mark: | :white_check_mark: |
| JetPack 6.1.x | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :white_check_mark: | :white_check_mark: | :white_check_mark: |
| JetPack 6.2.x | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :large_orange_diamond: | :white_check_mark: | :white_check_mark: | :white_check_mark: |

### FFmpeg support list

  - The library is currently compatible with all FFmpeg versions from 4.2 to 8.0+.
  - It may also work with versions older than 4.2, but it has not been tested.

### Supports Decoding
  - H.264/AVC (ffmpeg codec name: h264_nvmpi)
  - H.265/HEVC (ffmpeg codec name: hevc_nvmpi)
  - MPEG2 (ffmpeg codec name: mpeg2_nvmpi)
  - MPEG4 (ffmpeg codec name: mpeg4_nvmpi)
  - VP8 (ffmpeg codec name: vp8_nvmpi)
  - VP9 (ffmpeg codec name: vp9_nvmpi)
  
### Supports Encoding
  - H.264/AVC (ffmpeg codec name: h264_nvmpi)
  - H.265/HEVC (ffmpeg codec name: hevc_nvmpi)
  
### Other Features
  - Hardware accelerated video scaling during decoding

### Quick Start

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

For full build instructions, CMake options, cross-compilation, and usage examples see **[docs/BUILD.md](docs/BUILD.md)**.

### Documentation

- **[Build Guide](docs/BUILD.md)** — Complete build, installation, and usage instructions
- **[Development Guide](docs/DEVELOPMENT.md)** — Architecture, patch system, and how to add new FFmpeg versions
