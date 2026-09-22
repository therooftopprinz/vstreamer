# Decoder

Abstract decompress front. Inverse of [Encoder](component-encoder.md).
Rover JPEG uses `JpegDecoderMulticore` (internal worker pool). Not a source
or a sink.

```cpp
class Decoder {
public:
    virtual ~Decoder() = default;

    virtual const char *name() const = 0;
    virtual MediaKind input_kind() const = 0;   /* Mjpeg or H264 */
    virtual MediaKind output_kind() const = 0;  /* Nv12 */

    virtual int nv12_size(int width, int height) const = 0;

    virtual int open(const CodecParams &p) = 0;
    virtual void close() = 0;

    /*
     * Decompress one input AU. May emit 0..N frames via `emit`
     * (usually one). `in` remains caller-owned. Emitted NV12 is
     * packed, linesize == width; valid only during `emit` unless
     * the implementation sets `data_deleter` (then core owns it).
     */
    virtual int decode(const Frame &in, EmitFn emit, void *opaque) = 0;
    virtual int flush(EmitFn emit, void *opaque) = 0;

    virtual int configure(const char *key, const char *value) = 0;
    virtual int query(const char *key, char *value, size_t n) const = 0;
};
```

`CodecParams` / `EmitFn` are the same types as encoder
([component-encoder.md](component-encoder.md)). Shared `frame_s` /
`media_kind_e` are in core ([frame.hpp](../src/core/frame.hpp)).
`qp` / `gop` are unused
on decode; `width` / `height` / `fps` size the output NV12 (pad/crop
to the pipeline size, W ×32 when feeding Cedar).

`decode` of a JPEG (or H.264 AU) whose coded size is smaller than the
open size pads by repeating the last line/chroma row (rover
`pack_yuv*`). Unsupported `in.kind` or pix_fmt → negative, no emit.

Until this interface existed, JPEG decode lived in `camera.c` workers.
H.264 decode for the GS path did not exist in-tree (GStreamer
`avdec_h264`).

## Keys

| Key | Who |
|-----|-----|
| `size` | output `WxH` |
| `fps` | timestamp base; pass-through |
| `workers` | decode thread count for `JpegDecoderMulticore` (1–8, default 2); set only while closed |
| `output_mode` | `filter` (default) or `convert` — accept only native NV12-family formats, or also convert 422 → packed NV12 |
| `output_format` | target packed format; only `nv12` for now |
| `format` | query alias of `output_format` |

Shared helpers live in `src/core/output_opts.hpp`. Pixel pack/convert helpers live in
`src/core/pix_convert.hpp`. Encoder/source components may adopt the same names later.

**filter:** MPP `YUV420SP`/`VU` or libav `YUV420P` → packed NV12; other chroma → `-ENOTSUP`.
**convert:** also accept MPP `YUV422SP`/`VU` or libav `YUV422P` and convert to packed NV12 (8-bit only; 10-bit / YUV400 still unsupported).

`JpegDecoderMulticore` runs an internal pool (camera.c model): each worker owns a
single-threaded libav MJPEG context; `input` queues JPEG copies; `output` returns
frames in submit order.

## Implementations

| Class | `input_kind` | `output_kind` | Notes |
|-------|--------------|---------------|-------|
| `JpegDecoderMulticore` | `Mjpeg` | `Nv12` | CPU worker pool (default 2); `filter`=YUV420P only, `convert`=+YUV422P → packed NV12 |
| `H264Decoder` | `H264` | `Nv12` | MPP GS path; `filter`=YUV420SP, `convert`=+YUV422SP → packed NV12 |
| `NullDecoder` | — | — | source already emits NV12 or H.264 that sinks accept |

**As Source** (rover): `V4l2Source` (`Mjpeg`) → `JpegDecoderMulticore` →
`CedrusEncoder` → `RtpSink`.

**As Sink** (GS): `RtpSource` (`H264`) → `H264Decoder` → `DisplaySink`
(`Nv12`).
