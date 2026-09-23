#include "components/noise_source.hpp"

#include "core/time_util.hpp"

#include "core/key_util.hpp"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
}

namespace vstreamer
{
namespace
{

/* Random 1080p MJPEG can exceed 2 MiB at default qscale. */
constexpr size_t k_max_jpeg = 8ULL * 1024ULL * 1024ULL;

double now_sec()
{
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

uint32_t rng32(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x != 0 ? x : 1;
    return *state;
}

void fill_plane_rand(uint8_t *p, int linesize, int w, int h, uint32_t *rng)
{
    for (int y = 0; y < h; y++)
    {
        uint8_t *row = p + static_cast<size_t>(y) * static_cast<size_t>(linesize);
        int      x = 0;
        for (; x + 4 <= w; x += 4)
        {
            uint32_t r = rng32(rng);
            row[x] = static_cast<uint8_t>(r);
            row[x + 1] = static_cast<uint8_t>(r >> 8);
            row[x + 2] = static_cast<uint8_t>(r >> 16);
            row[x + 3] = static_cast<uint8_t>(r >> 24);
        }
        if (x < w)
        {
            uint32_t r = rng32(rng);
            for (; x < w; x++)
            {
                row[x] = static_cast<uint8_t>(r);
                r >>= 8;
            }
        }
    }
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
    if (ww < 2 || hh < 2 || (ww % 2) || (hh % 2) || ww > 7680 || hh > 4320)
    {
        return -EINVAL;
    }
    *w = static_cast<int>(ww);
    *h = static_cast<int>(hh);
    return 0;
}

}  // namespace

noise_source::noise_source() = default;

noise_source::~noise_source()
{
    close();
}

std::string noise_source::name() const
{
    return "noise";
}

media_kind_e noise_source::output_kind() const
{
    std::lock_guard<std::mutex> lock(mu);
    return output_nv12 ? media_kind_e::NV12 : media_kind_e::MJPEG;
}

int noise_source::ensure_encoder_locked()
{
    if (enc && enc_w == width && enc_h == height)
    {
        return 0;
    }
    free_encoder_locked();

    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (nullptr == codec)
    {
        return -ENOENT;
    }

    AVCodecContext *enc = avcodec_alloc_context3(codec);
    if (nullptr == enc)
    {
        return -ENOMEM;
    }
    enc->width = width;
    enc->height = height;
    enc->pix_fmt = AV_PIX_FMT_YUVJ420P;
    enc->time_base = AVRational{1, 1};
    enc->flags |= AV_CODEC_FLAG_QSCALE;
    /* Random snow MJPEG is huge at low q; favor throughput on bench / rover resolutions. */
    const int pixels = width * height;
    if (pixels >= 1920 * 1080)
    {
        enc->global_quality = 31 * FF_QP2LAMBDA;
    }
    else if (pixels >= 1280 * 720)
    {
        enc->global_quality = 24 * FF_QP2LAMBDA;
    }
    else
    {
        enc->global_quality = 10 * FF_QP2LAMBDA;
    }
    if (avcodec_open2(enc, codec, nullptr) < 0)
    {
        avcodec_free_context(&enc);
        return -EIO;
    }

    AVFrame *frame = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    if (nullptr == frame || nullptr == pkt)
    {
        av_frame_free(&frame);
        av_packet_free(&pkt);
        avcodec_free_context(&enc);
        return -ENOMEM;
    }
    frame->format = AV_PIX_FMT_YUVJ420P;
    frame->width = width;
    frame->height = height;
    if (av_frame_get_buffer(frame, 32) < 0)
    {
        av_frame_free(&frame);
        av_packet_free(&pkt);
        avcodec_free_context(&enc);
        return -ENOMEM;
    }

    this->enc = enc;
    this->avframe = frame;
    this->pkt = pkt;
    enc_w = width;
    enc_h = height;
    return 0;
}

void noise_source::free_encoder_locked()
{
    if (avframe)
    {
        AVFrame *f = static_cast<AVFrame *>(avframe);
        av_frame_free(&f);
        avframe = nullptr;
    }
    if (pkt)
    {
        AVPacket *p = static_cast<AVPacket *>(pkt);
        av_packet_free(&p);
        pkt = nullptr;
    }
    if (enc)
    {
        AVCodecContext *e = static_cast<AVCodecContext *>(enc);
        avcodec_free_context(&e);
        enc = nullptr;
    }
    enc_w = 0;
    enc_h = 0;
}

int noise_source::open()
{
    std::lock_guard<std::mutex> lock(mu);
    if (opened)
    {
        return 0;
    }
    if (!output_nv12)
    {
        int r = ensure_encoder_locked();
        if (r < 0)
        {
            return r;
        }
    }
    pts = 0;
    due_sec = 0;
    opened = true;
    return 0;
}

void noise_source::close()
{
    std::lock_guard<std::mutex> lock(mu);
    free_encoder_locked();
    opened = false;
    due_sec = 0;
}

int noise_source::make_nv12_locked(uint8_t **out, size_t *out_sz)
{
    *out = nullptr;
    *out_sz = 0;
    if (width < 2 || height < 2 || (width % 2) != 0 || (height % 2) != 0)
    {
        return -EINVAL;
    }

    const size_t y_sz = static_cast<size_t>(width) * static_cast<size_t>(height);
    const size_t nv12_sz = y_sz + y_sz / 2ULL;
    auto        *buf = static_cast<uint8_t *>(std::malloc(nv12_sz));
    if (nullptr == buf)
    {
        return -ENOMEM;
    }

    fill_plane_rand(buf, width, width, height, &rng);
    fill_plane_rand(buf + y_sz, width, width, height / 2, &rng);
    *out = buf;
    *out_sz = nv12_sz;
    return 0;
}

int noise_source::make_jpeg_locked(int64_t frame_pts, uint8_t **out, size_t *out_sz)
{
    *out = nullptr;
    *out_sz = 0;
    int r = ensure_encoder_locked();
    if (r < 0)
    {
        return r;
    }

    auto *enc = static_cast<AVCodecContext *>(this->enc);
    auto *frame = static_cast<AVFrame *>(this->avframe);
    auto *pkt = static_cast<AVPacket *>(this->pkt);

    if (av_frame_make_writable(frame) < 0)
    {
        return -EIO;
    }
    fill_plane_rand(frame->data[0], frame->linesize[0], width, height, &rng);
    fill_plane_rand(frame->data[1], frame->linesize[1], width / 2, height / 2, &rng);
    fill_plane_rand(frame->data[2], frame->linesize[2], width / 2, height / 2, &rng);
    frame->pts = frame_pts;

    if (avcodec_send_frame(enc, frame) < 0)
    {
        return -EIO;
    }
    av_packet_unref(pkt);
    if (avcodec_receive_packet(enc, pkt) < 0)
    {
        return -EIO;
    }
    if (pkt->size <= 0 || static_cast<size_t>(pkt->size) > k_max_jpeg)
    {
        std::fprintf(stderr, "noise_source: MJPEG size %d exceeds max %zu\n", pkt->size,
                     k_max_jpeg);
        return -EIO;
    }

    auto *buf = static_cast<uint8_t *>(std::malloc(static_cast<size_t>(pkt->size)));
    if (nullptr == buf)
    {
        return -ENOMEM;
    }
    std::memcpy(buf, pkt->data, static_cast<size_t>(pkt->size));
    *out = buf;
    *out_sz = static_cast<size_t>(pkt->size);
    return 0;
}

void noise_source::pace_locked(int timeout_ms)
{
    int fps = this->fps > 0 ? this->fps : 30;
    double period = 1.0 / static_cast<double>(fps);
    double t0 = now_sec();
    if (due_sec <= 0)
    {
        due_sec = t0 + period;
    }
    else
    {
        due_sec += period;
    }

    const double now = now_sec();
    double       wait = due_sec - now;
    if (wait < -1.0)
    {
        due_sec = now + period;
        wait = period;
    }
    if (wait <= 0)
    {
        return;
    }
    if (timeout_ms == 0)
    {
        return;
    }
    if (timeout_ms > 0)
    {
        double cap = static_cast<double>(timeout_ms) / 1000.0;
        if (wait > cap)
        {
            wait = cap;
        }
    }
    std::this_thread::sleep_for(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::duration<double>(wait)));
}

int noise_source::output(uint8_t /*port*/, data_packet &out, int timeout_ms)
{
    std::unique_lock<std::mutex> lock(mu);
    if (!opened)
    {
        return -EBADF;
    }

    const int64_t frame_pts = pts;
    pts++;

    if (output_nv12)
    {
        uint8_t *nv12 = nullptr;
        size_t   nsz = 0;
        int      r = make_nv12_locked(&nv12, &nsz);
        if (r < 0)
        {
            pts--;
            return r;
        }

        auto fd = std::make_unique<frame_data>();
        fd->kind = media_kind_e::NV12;
        fd->width = width;
        fd->height = height;
        fd->pts = frame_pts;
        fd->capture_mono_ns = steady_mono_ns();
        fd->key = true;
        fd->buf.reset(nv12, nsz, [](uint8_t *p) { std::free(p); });
        out.reset(std::move(fd));

        lock.unlock();
        pace_locked(timeout_ms);
        return 0;
    }

    uint8_t *jpeg = nullptr;
    size_t   jsz = 0;
    int      r = make_jpeg_locked(frame_pts, &jpeg, &jsz);
    if (r < 0)
    {
        pts--;
        return r;
    }

    auto fd = std::make_unique<frame_data>();
    fd->kind = media_kind_e::MJPEG;
    fd->width = width;
    fd->height = height;
    fd->pts = frame_pts;
    fd->capture_mono_ns = steady_mono_ns();
    fd->key = true;
    fd->buf.reset(jpeg, jsz, [](uint8_t *p) { std::free(p); });
    out.reset(std::move(fd));

    lock.unlock();
    pace_locked(timeout_ms);
    return 0;
}

int noise_source::configure(uint64_t /*key*/, int64_t /*value*/)
{
    return -EINVAL;
}

int noise_source::query(uint64_t /*key*/, int64_t * /*value*/) const
{
    return -EINVAL;
}

int noise_source::configure(std::string_view key, std::string_view *value)
{
    if (nullptr == value)
    {
        return -EINVAL;
    }
    std::string_view v = *value;

    std::lock_guard<std::mutex> lock(mu);
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
        int r = key_parse_i64(tmp.c_str(), &n);
        if (r < 0 || n <= 0 || n > 240)
        {
            return -EINVAL;
        }
        fps = static_cast<int>(n);
        return 0;
    }
    if (key == "format")
    {
        if (v == "nv12" || v == "NV12")
        {
            output_nv12 = true;
            return 0;
        }
        if (v != "mjpeg" && v != "mjpg" && v != "MJPEG" && v != "MJPG")
        {
            return -EINVAL;
        }
        output_nv12 = false;
        return 0;
    }
    return -EINVAL;
}

int noise_source::query(std::string_view key, std::string_view *value) const
{
    if (nullptr == value)
    {
        return -EINVAL;
    }

    std::lock_guard<std::mutex> lock(mu);
    if (key == "status")
    {
        query_buf = output_nv12 ? "noise_nv12" : "noise_mjpeg";
        *value = query_buf;
        return 0;
    }
    if (key == "size")
    {
        char buf[64];
        if (std::snprintf(buf, sizeof(buf), "%dx%d", width, height) < 0)
        {
            return -EINVAL;
        }
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
    if (key == "format")
    {
        query_buf = output_nv12 ? "nv12" : "mjpeg";
        *value = query_buf;
        return 0;
    }
    if (key == "device")
    {
        query_buf = "noise";
        *value = query_buf;
        return 0;
    }
    if (key == "media_type")
    {
        query_buf = output_nv12 ? "raw" : "mjpeg";
        *value = query_buf;
        return 0;
    }
    if (key == "pixel_type")
    {
        query_buf = output_nv12 ? "raw" : "mjpeg";
        *value = query_buf;
        return 0;
    }
    if (key == "width")
    {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d", width);
        query_buf = buf;
        *value = query_buf;
        return 0;
    }
    if (key == "height")
    {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d", height);
        query_buf = buf;
        *value = query_buf;
        return 0;
    }
    return -EINVAL;
}

}  // namespace vstreamer
