# vstreamer

Composable video pipeline framework (sources, codecs, sinks). See [docs/vstreamer.md](docs/vstreamer.md) for architecture and plugin contracts.

## Repository layout

```
vstreamer/
├── CMakeLists.txt
├── docs/
├── scripts/
└── src/
    ├── components/   # plugins (V4L2, JPEG/H.264, MKV, …)
    ├── core/         # frames, factory, component interfaces
    ├── network/      # stream sender/receiver (WIP)
    └── test_app/
```

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Component flags are CMake options (`ENABLE_V4L2_SOURCE`, `ENABLE_H264_DECODER_MPP`, etc.); see `CMakeLists.txt`.
