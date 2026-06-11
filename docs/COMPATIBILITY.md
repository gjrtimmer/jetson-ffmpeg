# Compatibility

Supported codecs, Jetson hardware, JetPack releases, and FFmpeg versions for
jetson-ffmpeg.

## Codec Support

| Codec        | Decoding | Encoding | FFmpeg codec name |
| ------------ | :------: | :------: | ----------------- |
| H.264 / AVC  |    ✅    |    ✅    | `h264_nvmpi`      |
| H.265 / HEVC |    ✅    |    ✅    | `hevc_nvmpi`      |
| MPEG-2       |    ✅    |    —     | `mpeg2_nvmpi`     |
| MPEG-4       |    ✅    |    —     | `mpeg4_nvmpi`     |
| VP8          |    ✅    |    —     | `vp8_nvmpi`       |
| VP9          |    ✅    |    —     | `vp9_nvmpi`       |

Hardware-accelerated video scaling during decoding is also supported.

## FFmpeg Support

jetson-ffmpeg is compatible with all FFmpeg versions from **4.2 up to 8.0+**.

- Dedicated patches are provided and CI-tested for FFmpeg
  **4.2**, **4.4**, **6.0**, **6.1**, **7.0**, **7.1**, and **8.0**.
- `scripts/ffpatch.sh` auto-detects the version of the FFmpeg tree it patches.
- Versions older than 4.2 may also work, but have not been tested.

## Jetson / JetPack Support

| Symbol | Meaning                                          |
| :----: | ------------------------------------------------ |
|   ✅   | Fully supported — tested on real hardware        |
|   🔵   | Untested — expected to work                      |
|   ➖   | No JetPack release available for this platform   |

The matrix is split by JetPack generation; platforms without a JetPack
release in a generation are omitted from that table.

### JetPack 5.x – 6.x

| JetPack | AGX Xavier | Xavier NX | AGX Orin | Orin NX | Orin Nano |
| ------- | :--------: | :-------: | :------: | :-----: | :-------: |
| 6.2.x   |     ➖     |    ➖     |    ✅    |   ✅    |    ✅     |
| 6.1.x   |     ➖     |    ➖     |    ✅    |   ✅    |    ✅     |
| 6.0.x   |     ➖     |    ➖     |    ✅    |   ✅    |    ✅     |
| 5.1.x   |     ✅     |    ✅     |    ✅    |   ✅    |    ✅     |
| 5.0.x   |     ✅     |    ✅     |    ✅    |   ➖    |    ➖     |

### JetPack 4.x

| JetPack | TX1 | TX2 | TX2i | Nano | AGX Xavier | Xavier NX |
| ------- | :-: | :-: | :--: | :--: | :--------: | :-------: |
| 4.6.x   | 🔵  | 🔵  |  🔵  |  ✅  |     ✅     |    ✅     |
| 4.5.x   | 🔵  | 🔵  |  🔵  |  ✅  |     ✅     |    ✅     |
| 4.4.x   | 🔵  | 🔵  |  🔵  |  🔵  |     🔵     |    🔵     |
| 4.3.x   | 🔵  | 🔵  |  🔵  |  🔵  |     🔵     |    ➖     |
| 4.2.x   | 🔵  | 🔵  |  🔵  |  🔵  |     🔵     |    ➖     |
| 4.1.x   | ➖  | ➖  |  ➖  |  ➖  |     🔵     |    ➖     |

### JetPack 1.x – 3.x (legacy)

| JetPack       | TK1 | TX1 | TX2 | TX2i |
| ------------- | :-: | :-: | :-: | :--: |
| 3.3.x         | ➖  | 🔵  | 🔵  |  🔵  |
| 3.2.x         | ➖  | 🔵  | 🔵  |  🔵  |
| 3.1.x         | 🔵  | 🔵  | 🔵  |  ➖  |
| 3.0.x         | 🔵  | 🔵  | 🔵  |  ➖  |
| 2.0.x – 2.3.x | 🔵  | 🔵  | ➖  |  ➖  |
| 1.0.x – 1.2.x | 🔵  | ➖  | ➖  |  ➖  |
