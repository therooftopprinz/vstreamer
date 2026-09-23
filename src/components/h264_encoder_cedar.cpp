#include "components/h264_encoder_cedar.hpp"

#include "core/key_util.hpp"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <utility>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

namespace vstreamer
{
namespace
{

constexpr size_t k_max_au = 2ULL * 1024ULL * 1024ULL;
constexpr size_t k_out_q_max = 32;

void nv12_keep(void * /*opaque*/, uint8_t * /*data*/)
{
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
    /* Cedar VE stride: width multiple of 32; both even. */
    if (ww < 32 || hh < 2 || (ww % 32) || (hh % 2) || ww > 7680 || hh > 4320)
    {
        return -EINVAL;
    }
    *w = static_cast<int>(ww);
    *h = static_cast<int>(hh);
    return 0;
}

}  // namespace

h264_encoder_cedar::h264_encoder_cedar() = default;

h264_encoder_cedar::~h264_encoder_cedar()
{
    close();
}

std::string h264_encoder_cedar::name() const
{
    return "h264_encoder_cedar";
}

media_kind_e h264_encoder_cedar::input_kind() const
{
    return media_kind_e::NV12;
}

media_kind_e h264_encoder_cedar::output_kind() const
{
    return media_kind_e::H264;
}

int h264_encoder_cedar::nv12_size_locked() const
{
    int w = live_w > 0 ? live_w : width;
    int h = live_h > 0 ? live_h : height;
    return av_image_get_buffer_size(AV_PIX_FMT_NV12, w, h, 1);
}

void h264_encoder_cedar::clear_out_locked()
{
    out_q.clear();
}

int h264_encoder_cedar::drain_packets_locked()
{
    auto *ctx = static_cast<AVCodecContext *>(this->ctx);
    auto *pkt = static_cast<AVPacket *>(this->pkt);
    if (nullptr == ctx || nullptr == pkt)
    {
        return -EBADF;
    }

    for (;;)
    {
        int ret = avcodec_receive_packet(ctx, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        {
            return 0;
        }
        if (ret < 0)
        {
            return ret;
        }

        if (pkt->size <= 0 || static_cast<size_t>(pkt->size) > k_max_au)
        {
            av_packet_unref(pkt);
            continue;
        }

        while (out_q.size() >= k_out_q_max)
        {
            out_q.pop_front();
        }

        auto *buf = static_cast<uint8_t *>(std::malloc(static_cast<size_t>(pkt->size)));
        if (nullptr == buf)
        {
            av_packet_unref(pkt);
            return -ENOMEM;
        }
        std::memcpy(buf, pkt->data, static_cast<size_t>(pkt->size));

        frame au;
        au.reset(media_kind_e::H264, live_w, live_h, pkt->pts,
                 !!(pkt->flags & AV_PKT_FLAG_KEY), buf, static_cast<size_t>(pkt->size),
                 [](uint8_t *p) { std::free(p); });
        out_q.push_back(std::move(au));
        av_packet_unref(pkt);
        cv.notify_one();
    }
}

int h264_encoder_cedar::codec_open_locked()
{
    const AVCodec *codec = avcodec_find_encoder_by_name("h264_cedrus");
    if (nullptr == codec)
    {
        std::fprintf(stderr, "h264_encoder_cedar: h264_cedrus not found\n");
        return -ENOENT;
    }

    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    AVFrame *frame = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    if (nullptr == ctx || nullptr == frame || nullptr == pkt)
    {
        av_frame_free(&frame);
        av_packet_free(&pkt);
        avcodec_free_context(&ctx);
        return -ENOMEM;
    }

    ctx->width = width;
    ctx->height = height;
    ctx->pix_fmt = AV_PIX_FMT_NV12;
    ctx->time_base = AVRational{1, fps};
    ctx->framerate = AVRational{fps, 1};
    ctx->gop_size = gop > 0 ? gop : 1;
    av_opt_set_int(ctx->priv_data, "qp", qp, 0);

    if (avcodec_open2(ctx, codec, nullptr) < 0)
    {
        std::fprintf(stderr,
                     "h264_encoder_cedar: open h264_cedrus failed (is /dev/cedar_dev free?)\n");
        av_frame_free(&frame);
        av_packet_free(&pkt);
        avcodec_free_context(&ctx);
        return -EIO;
    }

    this->ctx = ctx;
    this->avframe = frame;
    this->pkt = pkt;
    live_w = width;
    live_h = height;
    live_fps = fps;
    live_qp = qp;
    live_gop = gop;
    reopen_req = false;
    return 0;
}

void h264_encoder_cedar::codec_close_locked()
{
    if (ctx)
    {
        auto *c = static_cast<AVCodecContext *>(ctx);
        avcodec_send_frame(c, nullptr);
        (void)drain_packets_locked();
    }

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
    if (ctx)
    {
        AVCodecContext *c = static_cast<AVCodecContext *>(ctx);
        avcodec_free_context(&c);
        ctx = nullptr;
    }
    live_w = 0;
    live_h = 0;
    live_fps = 0;
    live_qp = 0;
    live_gop = 0;
}

int h264_encoder_cedar::reopen_if_needed_locked()
{
    if (!reopen_req && ctx)
    {
        return 0;
    }
    codec_close_locked();
    int r = codec_open_locked();
    if (r < 0)
    {
        return r;
    }
    std::fprintf(stderr, "h264_encoder_cedar: opened %dx%d@%d qp=%d gop=%d\n", live_w, live_h,
                 live_fps, live_qp, live_gop);
    return 0;
}

int h264_encoder_cedar::open()
{
    std::lock_guard<std::mutex> lock(mu);
    if (opened)
    {
        return 0;
    }
    int r = codec_open_locked();
    if (r < 0)
    {
        return r;
    }
    opened = true;
    std::fprintf(stderr, "h264_encoder_cedar: opened %dx%d@%d qp=%d gop=%d\n", live_w, live_h,
                 live_fps, live_qp, live_gop);
    return 0;
}

void h264_encoder_cedar::close()
{
    std::lock_guard<std::mutex> lock(mu);
    codec_close_locked();
    clear_out_locked();
    opened = false;
    reopen_req = false;
    cv.notify_all();
}

int h264_encoder_cedar::input(uint8_t /*port*/, const data_packet &in)
{
    const frame_data &f = data_packet::cast<frame_data>(in);
    if (f.kind != media_kind_e::NV12)
    {
        return -EINVAL;
    }

    std::lock_guard<std::mutex> lock(mu);
    if (!opened)
    {
        return -EBADF;
    }

    int r = reopen_if_needed_locked();
    if (r < 0)
    {
        return r;
    }

    if (f.width != live_w || f.height != live_h)
    {
        return -EINVAL;
    }
    int want = nv12_size_locked();
    if (want < 0 || static_cast<size_t>(want) != f.buf.size || nullptr == f.buf.data)
    {
        return -EINVAL;
    }

    auto *ctx = static_cast<AVCodecContext *>(this->ctx);
    auto *frame = static_cast<AVFrame *>(this->avframe);

    av_frame_unref(frame);
    int sz = av_image_fill_arrays(frame->data, frame->linesize, f.buf.data, AV_PIX_FMT_NV12,
                                  live_w, live_h, 1);
    if (sz < 0)
    {
        return sz;
    }
    frame->width = live_w;
    frame->height = live_h;
    frame->format = AV_PIX_FMT_NV12;
    frame->pts = f.pts;
    frame->buf[0] =
        av_buffer_create(f.buf.data, static_cast<size_t>(sz), nv12_keep, nullptr, 0);
    if (nullptr == frame->buf[0])
    {
        av_frame_unref(frame);
        return AVERROR(ENOMEM);
    }

    int ret = avcodec_send_frame(ctx, frame);
    if (ret < 0)
    {
        av_frame_unref(frame);
        return ret;
    }

    r = drain_packets_locked();
    av_frame_unref(frame);
    return r;
}

int h264_encoder_cedar::output(uint8_t /*port*/, data_packet &out, int timeout_ms)
{
    std::unique_lock<std::mutex> lock(mu);
    if (!opened && out_q.empty())
    {
        return -EBADF;
    }

    auto ready = [this]() { return !out_q.empty() || !opened; };

    if (out_q.empty())
    {
        if (timeout_ms == 0)
        {
            return -EAGAIN;
        }
        if (timeout_ms < 0)
        {
            cv.wait(lock, ready);
        }
        else
        {
            cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), ready);
        }
    }

    if (out_q.empty())
    {
        return opened ? -EAGAIN : -EBADF;
    }

    out.adopt_frame(std::move(out_q.front()));
    out_q.pop_front();
    return 0;
}

int h264_encoder_cedar::configure(uint64_t /*key*/, int64_t /*value*/)
{
    return -EINVAL;
}

int h264_encoder_cedar::query(uint64_t /*key*/, int64_t * /*value*/) const
{
    return -EINVAL;
}

int h264_encoder_cedar::configure(std::string_view key, std::string_view *value)
{
    if (nullptr == value)
    {
        return -EINVAL;
    }
    std::string_view v = *value;
    std::string tmp(v);

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
        if (w != width || h != height)
        {
            width = w;
            height = h;
            if (opened)
            {
                reopen_req = true;
            }
        }
        return 0;
    }
    if (key == "fps")
    {
        int64_t n = 0;
        if (key_parse_i64(tmp.c_str(), &n) < 0 || n < 1 || n > 120)
        {
            return -EINVAL;
        }
        int nv = static_cast<int>(n);
        if (nv != fps)
        {
            fps = nv;
            if (opened)
            {
                reopen_req = true;
            }
        }
        return 0;
    }
    if (key == "qp")
    {
        int64_t n = 0;
        if (key_parse_i64(tmp.c_str(), &n) < 0 || n < 2 || n > 47)
        {
            return -EINVAL;
        }
        int nv = static_cast<int>(n);
        if (nv != qp)
        {
            qp = nv;
            if (opened)
            {
                reopen_req = true;
            }
        }
        return 0;
    }
    if (key == "gop")
    {
        int64_t n = 0;
        if (key_parse_i64(tmp.c_str(), &n) < 0 || n < 1 || n > 255)
        {
            return -EINVAL;
        }
        int nv = static_cast<int>(n);
        if (nv != gop)
        {
            gop = nv;
            if (opened)
            {
                reopen_req = true;
            }
        }
        return 0;
    }
    return -EINVAL;
}

int h264_encoder_cedar::query(std::string_view key, std::string_view *value) const
{
    if (nullptr == value)
    {
        return -EINVAL;
    }

    std::lock_guard<std::mutex> lock(mu);

    if (key == "status")
    {
        query_buf = opened ? "open" : "closed";
        *value = query_buf;
        return 0;
    }
    if (key == "size")
    {
        char buf[64];
        int  w = live_w > 0 ? live_w : width;
        int  h = live_h > 0 ? live_h : height;
        if (std::snprintf(buf, sizeof(buf), "%dx%d", w, h) < 0)
        {
            return -EINVAL;
        }
        query_buf = buf;
        *value = query_buf;
        return 0;
    }
    if (key == "fps" || key == "qp" || key == "gop")
    {
        int n = 0;
        if (key == "fps")
        {
            n = live_fps > 0 ? live_fps : fps;
        }
        else if (key == "qp")
        {
            n = live_qp > 0 ? live_qp : qp;
        }
        else
        {
            n = live_gop > 0 ? live_gop : gop;
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
    if (key == "backend" || key == "codec")
    {
        query_buf = "h264_cedrus";
        *value = query_buf;
        return 0;
    }
    return -EINVAL;
}

}  // namespace vstreamer
