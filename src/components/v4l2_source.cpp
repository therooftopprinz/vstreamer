#include "components/v4l2_source.hpp"

#include "core/key_util.hpp"

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <unistd.h>

#include <linux/videodev2.h>

namespace vstreamer
{
namespace
{

constexpr size_t k_max_jpeg = 2 * 1024 * 1024;

double now_sec()
{
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

int xioctl(int fd, unsigned long req, void *arg)
{
    int ret = 0;
    do
    {
        ret = ::ioctl(fd, req, arg);
    } while (ret == -1 && errno == EINTR);
    return ret;
}

bool capture_lost_errno(int e)
{
    return e == ENODEV || e == EIO || e == ENXIO || e == EBADF;
}

void ctl_norm_name(std::string_view in, char *out, size_t n)
{
    size_t j = 0;
    for (char ch : in)
    {
        auto c = static_cast<unsigned char>(ch);
        if (j + 1 >= n)
        {
            break;
        }
        if (std::isalnum(c))
        {
            out[j++] = static_cast<char>(std::tolower(c));
        }
        else if (j > 0 && out[j - 1] != '_')
        {
            out[j++] = '_';
        }
    }
    while (j > 0 && out[j - 1] == '_')
    {
        j--;
    }
    out[j] = '\0';
}

int parse_size(std::string_view s, int *w, int *h)
{
    if (nullptr == w || nullptr == h || s.empty())
    {
        return -EINVAL;
    }
    char buf[64];
    if (s.size() >= sizeof(buf))
    {
        return -EINVAL;
    }
    std::memcpy(buf, s.data(), s.size());
    buf[s.size()] = '\0';
    char *x = std::strchr(buf, 'x');
    if (nullptr == x)
    {
        x = std::strchr(buf, 'X');
    }
    if (nullptr == x || x == buf || x[1] == '\0')
    {
        return -EINVAL;
    }
    *x = '\0';
    int64_t ww = 0;
    int64_t hh = 0;
    if (key_parse_i64(buf, &ww) < 0 || key_parse_i64(x + 1, &hh) < 0)
    {
        return -EINVAL;
    }
    if (ww < 2 || hh < 2 || (ww % 32) != 0 || ww > 7680 || hh > 4320)
    {
        return -EINVAL;
    }
    *w = static_cast<int>(ww);
    *h = static_cast<int>(hh);
    return 0;
}

void uvc_fix_defaults(int fd)
{
    struct v4l2_control c {};
    c.id = V4L2_CID_EXPOSURE_AUTO;
    c.value = 3; /* aperture priority */
    xioctl(fd, VIDIOC_S_CTRL, &c);
    c.id = V4L2_CID_SATURATION;
    c.value = 96;
    xioctl(fd, VIDIOC_S_CTRL, &c);
    c.id = 0x009a0903; /* exposure_dynamic_framerate */
    c.value = 0;
    xioctl(fd, VIDIOC_S_CTRL, &c);
}

bool eq_ci(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
    {
        return false;
    }
    for (size_t i = 0; i < a.size(); i++)
    {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
        {
            return false;
        }
    }
    return true;
}

}  // namespace

v4l2_source::v4l2_source()
{
    maps.assign(k_nbufs, nullptr);
    lengths.assign(k_nbufs, 0);
}

v4l2_source::~v4l2_source()
{
    close();
}

std::string v4l2_source::name() const
{
    return "v4l2";
}

media_kind_e v4l2_source::output_kind() const
{
    return media_kind_e::MJPEG;
}

void v4l2_source::capture_close_locked()
{
    if (!capture_open)
    {
        return;
    }
    if (fd >= 0)
    {
        enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(fd, VIDIOC_STREAMOFF, &t);
        for (unsigned i = 0; i < nbufs; i++)
        {
            if (maps[i])
            {
                ::munmap(maps[i], lengths[i]);
                maps[i] = nullptr;
                lengths[i] = 0;
            }
        }
        ::close(fd);
        fd = -1;
    }
    nbufs = 0;
    capture_open = false;
    live_w = 0;
    live_h = 0;
}

int v4l2_source::capture_open_locked(bool log_fail)
{
    if (!eq_ci(format, "mjpeg") && !eq_ci(format, "mjpg"))
    {
        if (log_fail)
        {
            std::fprintf(stderr, "v4l2_source: only format mjpeg is supported\n");
        }
        return -EINVAL;
    }

    int fd = ::open(device.c_str(), O_RDWR | O_NONBLOCK, 0);
    if (fd < 0)
    {
        int e = -errno;
        if (log_fail)
        {
            std::fprintf(stderr, "v4l2_source: open %s: %s\n", device.c_str(),
                         std::strerror(errno));
        }
        return e;
    }
    uvc_fix_defaults(fd);

    struct v4l2_format fmt {};
    int                want_w = width;
    int                want_h = height;
    bool               got = false;
    int                attempts = log_fail ? 20 : 5;
    for (int attempt = 0; attempt < attempts; attempt++)
    {
        std::memset(&fmt, 0, sizeof(fmt));
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = static_cast<unsigned>(want_w);
        fmt.fmt.pix.height = static_cast<unsigned>(want_h);
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
        fmt.fmt.pix.field = V4L2_FIELD_NONE;
        if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0)
        {
            int e = -errno;
            if (log_fail)
            {
                std::perror("v4l2_source S_FMT");
            }
            ::close(fd);
            return e;
        }
        if (fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_MJPEG &&
            static_cast<int>(fmt.fmt.pix.width) == want_w &&
            static_cast<int>(fmt.fmt.pix.height) == want_h)
        {
            got = true;
            break;
        }
        if (log_fail)
        {
            std::fprintf(stderr,
                         "v4l2_source: S_FMT %dx%d MJPEG, driver gave %dx%d "
                         "fmt=0x%08x (try %d)\n",
                         want_w, want_h, static_cast<int>(fmt.fmt.pix.width),
                         static_cast<int>(fmt.fmt.pix.height), fmt.fmt.pix.pixelformat,
                         attempt + 1);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!got || (static_cast<int>(fmt.fmt.pix.width) % 32) != 0)
    {
        if (log_fail)
        {
            std::fprintf(stderr,
                         "v4l2_source: capture %dx%d not usable (need MJPEG, width x32)\n",
                         static_cast<int>(fmt.fmt.pix.width),
                         static_cast<int>(fmt.fmt.pix.height));
        }
        ::close(fd);
        return -EINVAL;
    }

    width = static_cast<int>(fmt.fmt.pix.width);
    height = static_cast<int>(fmt.fmt.pix.height);

    struct v4l2_streamparm parm {};
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = fps > 0 ? fps : 30;
    xioctl(fd, VIDIOC_S_PARM, &parm);

    struct v4l2_requestbuffers req {};
    req.count = k_nbufs;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0)
    {
        int e = -errno;
        if (log_fail)
        {
            std::perror("v4l2_source REQBUFS");
        }
        ::close(fd);
        return e;
    }
    if (req.count == 0 || req.count > k_nbufs)
    {
        ::close(fd);
        return -ENOMEM;
    }

    nbufs = req.count;
    for (unsigned i = 0; i < nbufs; i++)
    {
        struct v4l2_buffer buf {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (xioctl(fd, VIDIOC_QUERYBUF, &buf) < 0)
        {
            int e = -errno;
            for (unsigned j = 0; j < i; j++)
            {
                if (maps[j])
                {
                    ::munmap(maps[j], lengths[j]);
                    maps[j] = nullptr;
                    lengths[j] = 0;
                }
            }
            ::close(fd);
            nbufs = 0;
            return e;
        }
        void *map =
            ::mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
        if (map == MAP_FAILED)
        {
            int e = -errno;
            for (unsigned j = 0; j < i; j++)
            {
                if (maps[j])
                {
                    ::munmap(maps[j], lengths[j]);
                    maps[j] = nullptr;
                    lengths[j] = 0;
                }
            }
            ::close(fd);
            nbufs = 0;
            return e;
        }
        maps[i] = map;
        lengths[i] = buf.length;
        if (xioctl(fd, VIDIOC_QBUF, &buf) < 0)
        {
            int e = -errno;
            for (unsigned j = 0; j <= i; j++)
            {
                if (maps[j])
                {
                    ::munmap(maps[j], lengths[j]);
                    maps[j] = nullptr;
                    lengths[j] = 0;
                }
            }
            ::close(fd);
            nbufs = 0;
            return e;
        }
    }

    enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd, VIDIOC_STREAMON, &t) < 0)
    {
        int e = -errno;
        for (unsigned i = 0; i < nbufs; i++)
        {
            if (maps[i])
            {
                ::munmap(maps[i], lengths[i]);
                maps[i] = nullptr;
                lengths[i] = 0;
            }
        }
        ::close(fd);
        nbufs = 0;
        return e;
    }

    this->fd = fd;
    capture_open = true;
    live_w = width;
    live_h = height;
    apply_ctrl_stash_locked();

    if (noise_active)
    {
        std::fprintf(stderr, "v4l2_source: capture restored; leaving noise\n");
        noise_active = false;
        noise.close();
    }
    if (log_fail)
    {
        std::fprintf(stderr, "v4l2_source: capture open %s %dx%d MJPEG@%d\n", device.c_str(),
                     width, height, fps);
    }
    return 0;
}

int v4l2_source::apply_ctrl_stash_locked()
{
    if (!capture_open)
    {
        return 0;
    }
    for (const auto &kv : ctrl_stash)
    {
        struct v4l2_control c {};
        c.id = kv.first;
        c.value = kv.second;
        xioctl(fd, VIDIOC_S_CTRL, &c);
    }
    return 0;
}

int v4l2_source::set_ctrl_locked(uint32_t id, int32_t value)
{
    ctrl_stash[id] = value;
    if (!capture_open)
    {
        return 0;
    }
    struct v4l2_control c {};
    c.id = id;
    c.value = value;
    if (xioctl(fd, VIDIOC_S_CTRL, &c) < 0)
    {
        return -errno;
    }
    return 0;
}

int v4l2_source::get_ctrl_locked(uint32_t id, int64_t *value) const
{
    if (nullptr == value)
    {
        return -EINVAL;
    }
    if (!capture_open)
    {
        auto it = ctrl_stash.find(id);
        if (it == ctrl_stash.end())
        {
            return -ENODEV;
        }
        *value = it->second;
        return 0;
    }
    struct v4l2_control c {};
    c.id = id;
    if (xioctl(fd, VIDIOC_G_CTRL, &c) < 0)
    {
        return -errno;
    }
    *value = c.value;
    return 0;
}

int v4l2_source::resolve_ctrl_name_locked(std::string_view name, uint32_t *id) const
{
    if (nullptr == id || name.empty())
    {
        return -EINVAL;
    }

    std::string tmp(name);
    char       *end = nullptr;
    errno = 0;
    unsigned long v = std::strtoul(tmp.c_str(), &end, 0);
    if (errno == 0 && end && *end == '\0' && v > 0)
    {
        *id = static_cast<uint32_t>(v);
        return 0;
    }

    if (!capture_open)
    {
        return -ENODEV;
    }

    char want[64];
    ctl_norm_name(name, want, sizeof(want));
    if (want[0] == '\0')
    {
        return -EINVAL;
    }

    struct v4l2_queryctrl qc {};
    qc.id = 0;
    for (;;)
    {
        qc.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
        if (xioctl(fd, VIDIOC_QUERYCTRL, &qc) < 0)
        {
            break;
        }
        if (qc.flags & V4L2_CTRL_FLAG_DISABLED)
        {
            continue;
        }
        char have[64];
        ctl_norm_name(reinterpret_cast<const char *>(qc.name), have, sizeof(have));
        if (std::strcmp(want, have) == 0)
        {
            *id = qc.id;
            return 0;
        }
    }
    return -EINVAL;
}

int v4l2_source::list_ctrls_locked(std::string *out) const
{
    if (nullptr == out)
    {
        return -EINVAL;
    }
    out->clear();
    if (!capture_open)
    {
        return -ENODEV;
    }

    struct v4l2_queryctrl qc {};
    qc.id = 0;
    bool first = true;
    for (;;)
    {
        qc.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
        if (xioctl(fd, VIDIOC_QUERYCTRL, &qc) < 0)
        {
            break;
        }
        if (qc.flags & V4L2_CTRL_FLAG_DISABLED)
        {
            continue;
        }
        struct v4l2_control c {};
        c.id = qc.id;
        if (xioctl(fd, VIDIOC_G_CTRL, &c) < 0)
        {
            continue;
        }
        char name[64];
        ctl_norm_name(reinterpret_cast<const char *>(qc.name), name, sizeof(name));
        if (name[0] == '\0')
        {
            continue;
        }
        char piece[96];
        int  m = std::snprintf(piece, sizeof(piece), "%s%s=%d", first ? "" : " ", name, c.value);
        if (m < 0 || static_cast<size_t>(m) >= sizeof(piece))
        {
            continue;
        }
        out->append(piece);
        first = false;
    }
    return 0;
}

int v4l2_source::open()
{
    std::lock_guard<std::mutex> lock(mu);
    if (source_open)
    {
        return 0;
    }

    /* Keep noise size at rover snow defaults; pace with capture fps. */
    {
        char         fps_buf[32];
        key_format_i64(fps, fps_buf, sizeof(fps_buf));
        std::string_view fps_sv = fps_buf;
        noise.configure("fps", &fps_sv);
        std::string_view size_sv = "416x240";
        noise.configure("size", &size_sv);
    }

    int r = capture_open_locked(true);
    if (r < 0)
    {
        int nr = noise.open();
        if (nr < 0)
        {
            return nr;
        }
        noise_active = true;
        cap_retry_due = now_sec() + 1.0;
        std::fprintf(stderr, "v4l2_source: capture unavailable; streaming noise 416x240@%d\n",
                     fps);
    }
    source_open = true;
    return 0;
}

void v4l2_source::close()
{
    std::lock_guard<std::mutex> lock(mu);
    capture_close_locked();
    noise.close();
    noise_active = false;
    source_open = false;
    cap_retry_due = 0;
}

bool v4l2_source::maybe_retry_capture_locked()
{
    double now = now_sec();
    if (now < cap_retry_due)
    {
        return false;
    }
    cap_retry_due = now + 1.0;
    if (capture_open_locked(false) == 0)
    {
        return true;
    }
    return false;
}

int v4l2_source::fetch_live_locked(frame &out, int timeout_ms)
{
    if (!capture_open || fd < 0)
    {
        return -EBADF;
    }

    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    struct timeval tv {};
    if (timeout_ms == 0)
    {
        tv.tv_sec = 0;
        tv.tv_usec = 0;
    }
    else if (timeout_ms < 0)
    {
        /* Match rover: 50ms select slice; caller loops. */
        tv.tv_sec = 0;
        tv.tv_usec = 50000;
    }
    else
    {
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
    }

    int sel = ::select(fd + 1, &fds, nullptr, nullptr, &tv);
    if (sel < 0)
    {
        int e = errno;
        if (capture_lost_errno(e))
        {
            std::fprintf(stderr, "v4l2_source: select lost: %s\n", std::strerror(e));
            capture_close_locked();
            return -e;
        }
        if (e == EINTR)
        {
            return -EAGAIN;
        }
        return -e;
    }
    if (sel == 0)
    {
        return -EAGAIN;
    }

    struct v4l2_buffer buf {};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_DQBUF, &buf) != 0)
    {
        int e = errno;
        if (capture_lost_errno(e))
        {
            std::fprintf(stderr, "v4l2_source: dqbuf lost: %s\n", std::strerror(e));
            capture_close_locked();
            return -e;
        }
        if (e == EAGAIN)
        {
            return -EAGAIN;
        }
        return -e;
    }

    int ret = 0;
    if (buf.bytesused > 0 && buf.bytesused <= k_max_jpeg && buf.index < nbufs)
    {
        size_t         size = buf.bytesused;
        const uint8_t *src = static_cast<const uint8_t *>(maps[buf.index]);
        auto          *tmp = static_cast<uint8_t *>(std::malloc(size));
        if (nullptr == tmp)
        {
            ret = -ENOMEM;
        }
        else
        {
            std::memcpy(tmp, src, size);
            out.reset(media_kind_e::MJPEG, live_w, live_h, pts++, true, tmp, size,
                      [](uint8_t *p) { std::free(p); });
            ret = 0;
        }
    }
    else
    {
        ret = -EAGAIN;
    }

    xioctl(fd, VIDIOC_QBUF, &buf);
    return ret;
}

int v4l2_source::output(uint8_t port, frame &out, int timeout_ms)
{
    std::unique_lock<std::mutex> lock(mu);
    if (!source_open)
    {
        return -EBADF;
    }

    if (!capture_open)
    {
        maybe_retry_capture_locked();
    }

    if (capture_open)
    {
        int r = fetch_live_locked(out, timeout_ms);
        if (r == 0)
        {
            return 0;
        }
        if (capture_open)
        {
            return r;
        }
        /* Lost device — fall through to noise. */
    }

    if (!noise_active)
    {
        int nr = noise.open();
        if (nr < 0)
        {
            return nr;
        }
        noise_active = true;
        std::fprintf(stderr, "v4l2_source: capture unavailable; streaming noise 416x240@%d\n",
                     fps);
    }
    /* Drop lock so console configure/query is not blocked by fps pacing. */
    lock.unlock();
    return noise.output(port, out, timeout_ms);
}

int v4l2_source::configure(uint64_t key, int64_t value)
{
    std::lock_guard<std::mutex> lock(mu);
    if (key == 0 || key > 0xffffffffu)
    {
        return -EINVAL;
    }
    return set_ctrl_locked(static_cast<uint32_t>(key), static_cast<int32_t>(value));
}

int v4l2_source::query(uint64_t key, int64_t *value) const
{
    std::lock_guard<std::mutex> lock(mu);
    if (key == 0 || key > 0xffffffffu)
    {
        return -EINVAL;
    }
    return get_ctrl_locked(static_cast<uint32_t>(key), value);
}

int v4l2_source::configure(std::string_view key, std::string_view *value)
{
    if (nullptr == value)
    {
        return -EINVAL;
    }
    std::string_view v = *value;

    std::lock_guard<std::mutex> lock(mu);
    if (key == "device")
    {
        if (v.empty() || v.size() >= 512)
        {
            return -EINVAL;
        }
        device.assign(v.begin(), v.end());
        return 0;
    }
    if (key == "format")
    {
        if (!eq_ci(v, "mjpeg") && !eq_ci(v, "mjpg"))
        {
            return -EINVAL;
        }
        format = "mjpeg";
        return 0;
    }
    if (key == "size")
    {
        int w = 0;
        int h = 0;
        int r = parse_size(v, &w, &h);
        if (r < 0)
        {
            return r;
        }
        width = w;
        height = h;
        return 0;
    }
    if (key == "fps")
    {
        int64_t n = 0;
        std::string tmp(v);
        if (key_parse_i64(tmp.c_str(), &n) < 0 || n <= 0 || n > 240)
        {
            return -EINVAL;
        }
        fps = static_cast<int>(n);
        char         fps_buf[32];
        key_format_i64(fps, fps_buf, sizeof(fps_buf));
        std::string_view fps_sv = fps_buf;
        noise.configure("fps", &fps_sv);
        return 0;
    }
    if (key.rfind("v4l2-ctl/", 0) == 0)
    {
        std::string_view ctl = key.substr(9);
        uint32_t         id = 0;
        int              r = resolve_ctrl_name_locked(ctl, &id);
        if (r < 0)
        {
            return r;
        }
        int64_t n = 0;
        std::string tmp(v);
        if (key_parse_i64(tmp.c_str(), &n) < 0)
        {
            return -EINVAL;
        }
        return set_ctrl_locked(id, static_cast<int32_t>(n));
    }
    return -EINVAL;
}

int v4l2_source::query(std::string_view key, std::string_view *value) const
{
    if (nullptr == value)
    {
        return -EINVAL;
    }

    std::lock_guard<std::mutex> lock(mu);
    if (key == "status")
    {
        if (capture_open)
        {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "live %dx%d %d", live_w, live_h, fps);
            query_buf = buf;
        }
        else
        {
            query_buf = "noise";
        }
        *value = query_buf;
        return 0;
    }
    if (key == "device")
    {
        query_buf = device;
        *value = query_buf;
        return 0;
    }
    if (key == "format")
    {
        query_buf = format;
        *value = query_buf;
        return 0;
    }
    if (key == "size")
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%dx%d", width, height);
        query_buf = buf;
        *value = query_buf;
        return 0;
    }
    if (key == "fps")
    {
        char buf[32];
        if (key_format_i64(fps, buf, sizeof(buf)) < 0)
        {
            return -EINVAL;
        }
        query_buf = buf;
        *value = query_buf;
        return 0;
    }
    if (key == "v4l2-ctl")
    {
        int r = list_ctrls_locked(&query_buf);
        if (r < 0)
        {
            return r;
        }
        *value = query_buf;
        return 0;
    }
    if (key.rfind("v4l2-ctl/", 0) == 0)
    {
        std::string_view ctl = key.substr(9);
        uint32_t         id = 0;
        int              r = resolve_ctrl_name_locked(ctl, &id);
        if (r < 0)
        {
            return r;
        }
        int64_t n = 0;
        r = get_ctrl_locked(id, &n);
        if (r < 0)
        {
            return r;
        }
        char buf[32];
        if (key_format_i64(n, buf, sizeof(buf)) < 0)
        {
            return -EINVAL;
        }
        query_buf = buf;
        *value = query_buf;
        return 0;
    }
    return -EINVAL;
}

}  // namespace vstreamer
