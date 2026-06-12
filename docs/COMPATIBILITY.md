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
|   ✅   | Tested on real hardware (CI)                     |
|   🔵   | Untested — expected to work                      |
|   ➖   | No JetPack release available for this platform   |

Hardware testing in CI currently covers a single device: **Orin NX on
JetPack 6 (L4T r36.4)**. All other platform/JetPack combinations are
untested in this project, even where the underlying code paths are the
same. The matrix is split by JetPack generation; platforms without a
JetPack release in a generation are omitted from that table.

### JetPack 5.x – 6.x

| JetPack | AGX Xavier | Xavier NX | AGX Orin | Orin NX | Orin Nano |
| ------- | :--------: | :-------: | :------: | :-----: | :-------: |
| 6.2.x   |     ➖     |    ➖     |    🔵    |   🔵    |    🔵     |
| 6.1.x   |     ➖     |    ➖     |    🔵    |   ✅    |    🔵     |
| 6.0.x   |     ➖     |    ➖     |    🔵    |   🔵    |    🔵     |
| 5.1.x   |     🔵     |    🔵     |    🔵    |   🔵    |    🔵     |
| 5.0.x   |     🔵     |    🔵     |    🔵    |   ➖    |    ➖     |

### JetPack 4.x

JetPack 4.x uses the legacy `nvbuf_utils` buffer API (instead of
`NvBufSurface` on JetPack 5+). The code supports it, but this build path
is not currently exercised by CI.

| JetPack | TX1 | TX2 | TX2i | Nano | AGX Xavier | Xavier NX |
| ------- | :-: | :-: | :--: | :--: | :--------: | :-------: |
| 4.6.x   | 🔵  | 🔵  |  🔵  |  🔵  |     🔵     |    🔵     |
| 4.5.x   | 🔵  | 🔵  |  🔵  |  🔵  |     🔵     |    🔵     |
| 4.4.x   | 🔵  | 🔵  |  🔵  |  🔵  |     🔵     |    🔵     |
| 4.3.x   | 🔵  | 🔵  |  🔵  |  🔵  |     🔵     |    ➖     |
| 4.2.x   | 🔵  | 🔵  |  🔵  |  🔵  |     🔵     |    ➖     |
| 4.1.x   | ➖  | ➖  |  ➖  |  ➖  |     🔵     |    ➖     |

### JetPack 3.2 – 3.3 (legacy)

The Jetson Multimedia API that libnvmpi is built on (V4L2 codecs +
`nvbuf_utils`, shipped as `tegra_multimedia_api`) first appeared around
L4T r28 / JetPack 3.2. Earlier JetPack releases (1.x – 3.1, and the TK1
platform entirely) do not provide this API and **cannot** be supported.

| JetPack | TX1 | TX2 | TX2i |
| ------- | :-: | :-: | :--: |
| 3.3.x   | 🔵  | 🔵  |  🔵  |
| 3.2.x   | 🔵  | 🔵  |  🔵  |
