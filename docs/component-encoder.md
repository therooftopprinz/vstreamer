# Encoder

Abstract compress front. The core owns **one** encoder slot. Not a
source or a sink. First backend is Cedar `h264_cedrus`
([vstreamer.md](vstreamer.md)).

Thread: `encode` / `flush` / `open` / `close` on the encode thread.
`configure` may run on the console thread; the implementation sets a
reopen flag — the encode thread is the one that actually `close`+`open`.

```cpp
struct CodecParams {
    int width  = 0;
    int height = 0;
    int fps    = 30;
    int qp     = 36;  /* 2..47 */
    int gop    = 30;  /* 1..255 */
};

using EmitFn = int (*)(void *opaque, const Frame &au);

class Encoder {
public:
    virtual ~Encoder() = default;

    virtual const char *name() const = 0;
    virtual MediaKind input_kind() const = 0;   /* Nv12 */
    virtual MediaKind output_kind() const = 0;  /* H264 */

    virtual int nv12_size(int width, int height) const = 0;

    /* 0 = ok. Packed NV12, linesize == width (Cedar VE stride). */
    virtual int open(const CodecParams &p) = 0;
    virtual void close() = 0;

    /*
     * Compress one input frame. May emit 0..N output AUs via `emit`
     * before return (same as rover encoder.h). `in` remains caller-owned.
     * Emitted frames are valid only for the duration of `emit`.
     */
    virtual int encode(const Frame &in, EmitFn emit, void *opaque) = 0;
    virtual int flush(EmitFn emit, void *opaque) = 0;

    /*
     * qp 2..47, gop 1..255. May be deferred until next encode
     * (Cedar PPS is written once at open — live av_opt corrupts the
     * bitstream). -EINVAL if out of range.
     */
    virtual int configure(const char *key, const char *value) = 0;
    virtual int query(const char *key, char *value, size_t n) const = 0;
};
```

`open` holds the hardware node (`/dev/cedar_dev`) until `close`, even
when RTP is gated. `flush` then `close` before a param reopen.

`encode` requires `in.kind == input_kind()`, `in.size == nv12_size(w,h)`,
and `w,h` matching the last `open`. Mismatch → skip/error, do not crash.

`emit` returning non-zero aborts that encode; the encoder still owns its
codec context.

## Keys

| Key | Range | Notes |
|-----|-------|-------|
| `qp` | 2..47 | reopen |
| `gop` | 1..255 | reopen |
| `fps` | 1..120 | reopen |
| `size` | `WxH` | reopen; W ×32 for Cedar |

Feedback writes the same keys; it does not call `open` itself.

## Implementations

| Class | `name()` | Notes |
|-------|----------|-------|
| `CedrusEncoder` | `poc` or `stock` | `encoder_cedrus.c`; build `ENCODER=poc\|stock` |
| `h264_encoder_intel` | `h264_encoder_intel` | NV12 → H.264 via FFmpeg `h264_vaapi` |
| `NullEncoder` | `none` | pass-through not used; graph with no encode slot |

This interface replaces rover `encoder.h` (`h264_enc_open` / `_encode` /
`_flush` / `_close` / `_nv12_size` / `_backend`). The callback shape is
the same; `frame_s` (core) wraps the old `(data, size, pts, key)` tuple.
