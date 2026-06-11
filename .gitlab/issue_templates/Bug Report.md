## Bug Report

### Description

<!-- Clear description of the bug. What happened? What did you expect? -->

### Steps to reproduce

1. 
2. 
3. 

### FFmpeg command

```bash
<!-- The exact ffmpeg command that triggers the issue (if applicable) -->
```

### Error output

```
<!-- Relevant error messages, logs, or stack traces -->
```

### Environment

- **Jetson model**: <!-- e.g. Orin NX 16GB, AGX Orin 64GB, Orin Nano 8GB -->
- **JetPack / L4T version**: <!-- e.g. JetPack 6.1 / L4T r36.4.0 -->
- **FFmpeg version**: <!-- output of ffmpeg -version (first line) -->
- **jetson-ffmpeg commit**: <!-- git commit hash or tag -->
- **Build method**: <!-- ffpatch.sh / CMake / Docker / pre-built -->

### Affected codec(s)

- [ ] h264_nvmpi (encoder)
- [ ] h264_nvmpi (decoder)
- [ ] hevc_nvmpi (encoder)
- [ ] hevc_nvmpi (decoder)
- [ ] mpeg2_nvmpi (decoder)
- [ ] mpeg4_nvmpi (decoder)
- [ ] vp8_nvmpi (decoder)
- [ ] vp9_nvmpi (decoder)
- [ ] Not codec-specific

### Additional context

<!-- Any other relevant information, workarounds tried, related issues -->

/label ~bug
