#include "components/mkv_sink.hpp"

#include "core/key_util.hpp"

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/time.h>
}

namespace vstreamer
{
namespace
{

double now_sec()
{
    return static_cast<double>(av_gettime()) / 1000000.0;
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
    if (ww < 16 || hh < 16 || ww > 7680 || hh > 4320)
    {
        return -EINVAL;
    }
    *w = static_cast<int>(ww);
    *h = static_cast<int>(hh);
    return 0;
}

}  // namespace

mkv_sink::mkv_sink() = default;

mkv_sink::~mkv_sink()
{
    close();
}

std::string mkv_sink::name() const
{
    return "mkv_sink";
}

media_kind_e mkv_sink::input_kind() const
{
    return media_kind_e::MJPEG;
}

void mkv_sink::stop_locked()
{
    if (!recording)
    {
        return;
    }

    auto *oc = static_cast<AVFormatContext *>(fmt);
    if (nullptr != oc)
    {
        av_write_trailer(oc);
        if (!(oc->oformat->flags & AVFMT_NOFILE))
        {
            avio_closep(&oc->pb);
        }
        avformat_free_context(oc);
    }

    fmt = nullptr;
    stream = nullptr;
    pts = 0;
    t0 = 0.0;
    live_w = 0;
    live_h = 0;
    recording = false;
}

int mkv_sink::start_locked(int w, int h)
{
    if (output_path.empty())
    {
        return -EINVAL;
    }

    stop_locked();

    AVFormatContext *oc = nullptr;
    if (avformat_alloc_output_context2(&oc, nullptr, "matroska", output_path.c_str()) < 0 || nullptr == oc)
    {
        return -EIO;
    }

    AVStream *st = avformat_new_stream(oc, nullptr);
    if (nullptr == st)
    {
        avformat_free_context(oc);
        return -EIO;
    }

    const int use_fps = fps > 0 ? fps : 30;
    st->time_base = AVRational{1, use_fps};
    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id = AV_CODEC_ID_MJPEG;
    st->codecpar->width = w;
    st->codecpar->height = h;
    st->codecpar->format = AV_PIX_FMT_YUVJ420P;

    if (!(oc->oformat->flags & AVFMT_NOFILE))
    {
        if (avio_open(&oc->pb, output_path.c_str(), AVIO_FLAG_WRITE) < 0)
        {
            avformat_free_context(oc);
            return -EIO;
        }
    }

    AVDictionary *mux_opts = nullptr;
    av_dict_set(&mux_opts, "live", "1", 0);
    if (avformat_write_header(oc, &mux_opts) < 0)
    {
        av_dict_free(&mux_opts);
        if (!(oc->oformat->flags & AVFMT_NOFILE))
        {
            avio_closep(&oc->pb);
        }
        avformat_free_context(oc);
        return -EIO;
    }
    av_dict_free(&mux_opts);

    fmt = oc;
    stream = st;
    live_w = w;
    live_h = h;
    pts = 0;
    t0 = now_sec();
    recording = true;
    return 0;
}

int mkv_sink::ensure_session_locked(int w, int h)
{
    if (output_path.empty())
    {
        return 0;
    }

    if (cfg_width > 0 && cfg_height > 0)
    {
        w = cfg_width;
        h = cfg_height;
    }

    if (w <= 0 || h <= 0)
    {
        return -EINVAL;
    }

    if (recording && (w != live_w || h != live_h))
    {
        stop_locked();
    }

    if (!recording)
    {
        return start_locked(w, h);
    }
    return 0;
}

int mkv_sink::write_frame_locked(const data_packet &in)
{
    auto *oc = static_cast<AVFormatContext *>(fmt);
    auto *st = static_cast<AVStream *>(stream);

    AVPacket *pkt = av_packet_alloc();
    if (nullptr == pkt)
    {
        return -ENOMEM;
    }

    const frame_data &f = data_packet::cast<frame_data>(in);

    uint8_t *buf = static_cast<uint8_t *>(av_malloc(f.buf.size));
    if (nullptr == buf)
    {
        av_packet_free(&pkt);
        return -ENOMEM;
    }
    std::memcpy(buf, f.buf.data, f.buf.size);

    pkt->data = buf;
    pkt->size = static_cast<int>(f.buf.size);
    pkt->pts = pts;
    pkt->dts = pts;
    pkt->duration = 1;
    pkt->stream_index = st->index;
    pkt->flags |= AV_PKT_FLAG_KEY;
    pkt->buf = av_buffer_create(buf, f.buf.size, av_buffer_default_free, nullptr, 0);
    if (nullptr == pkt->buf)
    {
        av_free(buf);
        av_packet_free(&pkt);
        return -ENOMEM;
    }

    int ret = av_interleaved_write_frame(oc, pkt);
    av_packet_free(&pkt);
    if (ret < 0)
    {
        return -EIO;
    }

    ++pts;
    ++frames_out;
    return 0;
}

int mkv_sink::open()
{
    std::lock_guard<std::mutex> lock(mu);
    opened = true;
    return 0;
}

void mkv_sink::close()
{
    std::lock_guard<std::mutex> lock(mu);
    stop_locked();
    opened = false;
}

int mkv_sink::input(uint8_t /*port*/, const data_packet &in)
{
    const frame_data &f = data_packet::cast<frame_data>(in);
    if (f.kind != media_kind_e::MJPEG)
    {
        return -EINVAL;
    }

    std::lock_guard<std::mutex> lock(mu);
    if (!opened)
    {
        return 0;
    }

    int w = f.width;
    int h = f.height;
    int r = ensure_session_locked(w, h);
    if (r < 0)
    {
        return r;
    }
    if (!recording)
    {
        return 0;
    }

    return write_frame_locked(in);
}

int mkv_sink::configure(uint64_t /*key*/, int64_t /*value*/)
{
    return -EINVAL;
}

int mkv_sink::query(uint64_t /*key*/, int64_t * /*value*/) const
{
    return -EINVAL;
}

int mkv_sink::configure(std::string_view key, std::string_view *value)
{
    if (nullptr == value)
    {
        return -EINVAL;
    }
    std::string_view v = *value;

    std::lock_guard<std::mutex> lock(mu);

    if (key == "output")
    {
        if (v.empty())
        {
            stop_locked();
            output_path.clear();
            return 0;
        }
        if (v.size() >= 4096)
        {
            return -EINVAL;
        }
        if (output_path != v)
        {
            stop_locked();
            output_path.assign(v.data(), v.size());
        }
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
        cfg_width = w;
        cfg_height = h;
        if (recording && (w != live_w || h != live_h))
        {
            stop_locked();
        }
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
        if (recording)
        {
            stop_locked();
        }
        return 0;
    }

    return -EINVAL;
}

int mkv_sink::query(std::string_view key, std::string_view *value) const
{
    if (nullptr == value)
    {
        return -EINVAL;
    }

    std::lock_guard<std::mutex> lock(mu);

    if (key == "output")
    {
        query_buf = output_path;
        *value = query_buf;
        return 0;
    }

    if (key == "stats")
    {
        if (recording)
        {
            double elapsed = now_sec() - t0;
            if (elapsed < 0.0)
            {
                elapsed = 0.0;
            }
            int mins = static_cast<int>(elapsed) / 60;
            int secs = static_cast<int>(elapsed) % 60;
            query_buf.resize(64);
            std::snprintf(query_buf.data(), query_buf.size(), "recording %d:%02d frames=%" PRIu64, mins, secs,
                          frames_out);
            query_buf.resize(std::strlen(query_buf.c_str()));
        }
        else
        {
            query_buf = "idle";
        }
        *value = query_buf;
        return 0;
    }

    return -EINVAL;
}

}  // namespace vstreamer
