# Source

Abstract capture front. The core pulls frames; it does not know V4L2.
First implementations: `v4l2_source`, `noise_source`. `rtp_source` is later.

Thread: `output` on the pipeline fetch thread. `configure` / `query` may
run on the console thread at the same time — the implementation locks.

Shared `media_kind_e` / `frame_s` live in core
([frame.hpp](../src/core/frame.hpp)). Source header:
[component_source.hpp](../src/core/component_source.hpp).

```cpp
class source {
public:
    virtual ~source() = default;

    virtual const char *name() const = 0;
    virtual media_kind_e output_kind() const = 0;

    /* 0 = ok. Negative = error (errno-style). */
    virtual int open() = 0;
    virtual void close() = 0;

    /*
     * Next frame. timeout_ms < 0 blocks, 0 polls, > 0 waits.
     * 0 = filled `out` (caller must frame_release).
     * -EAGAIN = no frame this wait.
     * Other negative = fatal for this attempt (core may close+open / noise).
     */
    virtual int output(uint8_t port, frame_s &out, int timeout_ms) = 0;

    /* Unknown key → -EINVAL. Integer query writes `*value`. */
    virtual int configure(const char *key, const char *value) = 0;
    virtual int configure(const char *key, int64_t value);
    virtual int configure(uint64_t key, const char *value);
    virtual int configure(uint64_t key, int64_t value);

    virtual int query(const char *key, char *value, size_t n) const = 0;
    virtual int query(const char *key, int64_t *value) const;
    virtual int query(uint64_t key, char *value, size_t n) const;
    virtual int query(uint64_t key, int64_t *value) const;
};
```


`open` after `configure`. To apply device/size changes or recover from
capture loss, callers `close` then `open` with current keys. `close` is
idempotent.

`output` always returns an **owned** copy (`data_deleter` set). mmap
capture copies out before `QBUF`. Do not return a pointer into driver
memory.

## Key / value types

`configure` / `query` are a 2×2 of key type × value type. Name keys are
`const char *`. Integer keys are `uint64_t`. Config file and console
still pass strings; core may parse a decimal / `0x` token into an
integer key.

| Key | Value in (`configure`) | Value out (`query`) |
|-----|------------------------|---------------------|
| `const char *` | `const char *` | `char *` + `size_t n` |
| `const char *` | `int64_t` | `int64_t *` |
| `uint64_t` | `const char *` | `char *` + `size_t n` |
| `uint64_t` | `int64_t` | `int64_t *` |

Defaults (override when the native type is not a string):

- `(name, int64)` formats decimal and calls `(name, str)`
- `(id, str)` parses base-0 (`40`, `0x00980913`) and calls `(id, int64)`
- `(name, int64 *)` queries the string and parses
- `(id, str out)` queries `int64` and formats decimal
- `(id, int64)` / `(id, int64 *)` return `-EINVAL` unless the plugin
  has integer keys

`v4l2_source` integer keys are live V4L2 control ids (`V4L2_CID_*` /
`0x00980913`). `noise_source` has none.

## Keys

| Key | Example | Who |
|-----|---------|-----|
| `device` | `/dev/video0` | `v4l2_source` |
| `format` | `mjpeg` | `v4l2_source` |
| `size` | `1280x720` | capture / noise (W ×32 for Cedar) |
| `fps` | `30` | capture / noise |
| `v4l2-ctl/<name>` | `v4l2-ctl/gain` = `40` | `v4l2_source` |
| `uint64_t` CID | `configure(0x00980913, 40)` | `v4l2_source` |

`query("status")` → `live` or `noise` (and optionally `WxH fps`).
`query("v4l2-ctl/<name>")` → current value as decimal.
`query("v4l2-ctl")` → all readable `name=val` on the open device.
`query(cid, &v)` → current control as `int64_t`.

`<name>` is the `v4l2-ctl --list-ctrls` string (normalized) or a numeric
id (`0x00980913`) in the name-key form. Not a uapi token. Integer /
bool / menu are `int64_t` or decimal strings. Unknown name or id, or a
source with no V4L2 node → `-EINVAL`.

`v4l2-ctl/*` and CID keys are a live ioctl when the device is open (no
capture close+open). If `configure` runs before `open`, stash and apply
after STREAMON. A later `open` reapplies the stash. Enumerate the live
device; do not hard-code a CID table.

## Implementations

| Class | `output_kind` | Notes |
|-------|---------------|-------|
| `v4l2_source` | `mjpeg` | mmap, 8 bufs; MJPEG only; width ×32 |
| `noise_source` | `mjpeg` | 416×240 snow; same `output` |

A source does not encode H.264, packetize for air, or talk to the radio.
`v4l2_source` may internally fall back to `noise_source` on `ENODEV` /
`EIO` / `ENXIO` / `EBADF` and retry `device` ~1 Hz — still one `source`
instance from the core’s point of view.
