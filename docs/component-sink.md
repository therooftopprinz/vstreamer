# Sink

Abstract destination. The core writes frames at a pipeline stage;
several sinks may be attached. Implementations: [vstreamer.md](vstreamer.md).

Thread: `write` on the producer thread for that stage (fetch thread for
pre-encode MJPEG, encode thread for H.264). `configure` / `set_enabled`
may run on the console thread — the implementation locks.

`write` must not block the source for more than a short enqueue. A
disabled sink returns 0 without consuming pipeline progress.

```cpp
class Sink {
public:
    virtual ~Sink() = default;

    virtual const char *name() const = 0;
    virtual MediaKind input_kind() const = 0;

    virtual int open() = 0;
    virtual void close() = 0;

    /*
     * One access unit. Caller still owns `in` (do not frame_release).
     * Copy if the sink needs the bytes after return.
     * 0 = ok or deliberately dropped (gated). Negative = error.
     */
    virtual int write(const Frame &in) = 0;

    /*
     * Send gate. timeout_ms < 0 ignored, 0 = on with no deadline,
     * > 0 = on until now + timeout (renew by calling again).
     * Record / display may no-op and return 0.
     */
    virtual int set_enabled(bool on, int timeout_ms) = 0;
    virtual bool enabled() const = 0;

    virtual int configure(const char *key, const char *value) = 0;
    virtual int query(const char *key, char *value, size_t n) const = 0;
};
```

`open` after `configure`. Core calls `set_enabled(false, 0)` at start
for sinks that honor the gate (`RtpSink`). `stream_request` is core +
feedback calling `set_enabled(true, timeout_ms)` — the sink does not
talk to the operator console itself.

## Keys

| Key | Example | Who |
|-----|---------|-----|
| `stream` | `127.0.0.1:22081` | `RtpSink` |
| `mtu` | `1400` | `RtpSink` (200..1500) |
| `output` | `/tmp/rover.mp4` | `Mp4Sink` (empty = idle) |
| `listen` | — | not a sink key (`RtpSource`) |

`query("stats")` → `pkts=<n> bytes=<n>` (RTP) or `recording <m>:<s>` /
`idle` (MP4). `query("addr")` → current `host:port` or `none`.

## Implementations

| Class | `input_kind` | Gate | Notes |
|-------|--------------|------|-------|
| `RtpSink` | `H264` | yes | PT 96, 90 kHz, SSRC `0xC0DE0001`, FU-A, cache SPS/PPS, prepend before slices |
| `Mp4Sink` | `Mjpeg` | no | mux JPEG; `configure("output", path)` / empty |
| `DisplaySink` | `Nv12` | no | GS preview; [usecase.md](usecase.md) *As Sink* |

`RtpSink` may have the UDP socket open while `enabled() == false` (boot
default). `write` then returns 0 and sends nothing.

`frame_s::kind` must match `input_kind()` or `write` returns `-EINVAL`.
Shared `frame_s` is in core ([frame.hpp](../src/core/frame.hpp)).
Core routes by kind: MJPEG → record sinks, H.264 → RTP, NV12 → display.
