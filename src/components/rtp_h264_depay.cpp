#include "components/rtp_h264_depay.hpp"

#include "core/fec_stream_header.hpp"
#include "core/key_util.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace vstreamer
{

rtp_h264_depay::rtp_h264_depay() = default;

rtp_h264_depay::~rtp_h264_depay()
{
    close();
}

std::string rtp_h264_depay::name() const
{
    return "rtp_h264_depay";
}

media_kind_e rtp_h264_depay::input_kind() const
{
    return media_kind_e::UNKNOWN;
}

media_kind_e rtp_h264_depay::output_kind() const
{
    return media_kind_e::H264;
}

packet_kind_e rtp_h264_depay::input_packet_kind() const
{
    return packet_kind_e::SOCK;
}

packet_kind_e rtp_h264_depay::output_packet_kind() const
{
    return packet_kind_e::FRAME;
}

int rtp_h264_depay::open()
{
    std::lock_guard<std::mutex> lock(mu);
    depay = rtp_h264_depacketizer(fps);
    au_ready = false;
    opened = true;
    return 0;
}

void rtp_h264_depay::close()
{
    std::lock_guard<std::mutex> lock(mu);
    depay.reset();
    au_buf.clear();
    au_ready = false;
    opened = false;
}

int rtp_h264_depay::input(uint8_t port, const data_packet &in)
{
    if (0 != port)
    {
        return -EINVAL;
    }
    const sock_data &s = data_packet::cast<sock_data>(in);
    std::lock_guard<std::mutex> lock(mu);
    std::vector<uint8_t>        au;
    const uint8_t *             feed_ptr = s.buf.data;
    size_t                      feed_len = s.buf.size;
    if (feed_len >= k_fec_stream_header_len)
    {
        feed_ptr += k_fec_stream_header_len;
        feed_len -= k_fec_stream_header_len;
    }
    const int ready = depay.feed(feed_ptr, feed_len, &au);
    if (ready < 0)
    {
        return ready;
    }
    if (1 == ready)
    {
        au_buf = std::move(au);
        au_pts = depay.au_pts();
        au_capture_mono_ns = depay.au_capture_mono_ns();
        au_ready = true;
    }
    return 0;
}

int rtp_h264_depay::output(uint8_t port, data_packet &out, int /*timeout_ms*/)
{
    if (0 != port)
    {
        return -EINVAL;
    }

    std::lock_guard<std::mutex> lock(mu);
    if (!au_ready)
    {
        return -EAGAIN;
    }

    uint8_t *buf = static_cast<uint8_t *>(std::malloc(au_buf.size()));
    if (nullptr == buf)
    {
        return -ENOMEM;
    }
    std::memcpy(buf, au_buf.data(), au_buf.size());
    auto fd = std::make_unique<frame_data>();
    fd->kind = media_kind_e::H264;
    fd->width = 0;
    fd->height = 0;
    fd->pts = au_pts;
    fd->capture_mono_ns = au_capture_mono_ns;
    fd->key = true;
    fd->buf.reset(buf, au_buf.size(), default_data_deleter);
    out.reset(std::move(fd));
    au_ready = false;
    return 0;
}

int rtp_h264_depay::configure(uint64_t /*key*/, int64_t /*value*/)
{
    return -ENOTSUP;
}

int rtp_h264_depay::query(uint64_t /*key*/, int64_t * /*value*/) const
{
    return -ENOTSUP;
}

int rtp_h264_depay::configure(std::string_view key, std::string_view *value)
{
    if (nullptr == value)
    {
        return -EINVAL;
    }

    if ("fps" == key)
    {
        int64_t v = 0;
        char    buf[32];
        std::memcpy(buf, value->data(), value->size());
        buf[value->size()] = '\0';
        if (key_parse_i64(buf, &v) < 0 || v < 1 || v > 120)
        {
            return -EINVAL;
        }
        std::lock_guard<std::mutex> lock(mu);
        fps = static_cast<int>(v);
        if (opened)
        {
            depay = rtp_h264_depacketizer(fps);
        }
        return 0;
    }
    return -ENOTSUP;
}

int rtp_h264_depay::query(std::string_view key, std::string_view *value) const
{
    if (nullptr == value)
    {
        return -EINVAL;
    }

    if ("loss" == key)
    {
        std::lock_guard<std::mutex> lock(mu);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.6f", static_cast<double>(depay.packet_loss()));
        query_buf = buf;
        *value = query_buf;
        return 0;
    }
    return -ENOTSUP;
}

}  // namespace vstreamer
