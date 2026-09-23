#include "components/rtp_h264_pay.hpp"

#include "core/key_util.hpp"

#include <cerrno>
#include <cstring>

namespace vstreamer
{

rtp_h264_pay::rtp_h264_pay() : pool(1500, 64) {}

rtp_h264_pay::~rtp_h264_pay()
{
    close();
}

std::string rtp_h264_pay::name() const
{
    return "rtp_h264_pay";
}

media_kind_e rtp_h264_pay::input_kind() const
{
    return media_kind_e::H264;
}

media_kind_e rtp_h264_pay::output_kind() const
{
    return media_kind_e::UNKNOWN;
}

packet_kind_e rtp_h264_pay::input_packet_kind() const
{
    return packet_kind_e::FRAME;
}

packet_kind_e rtp_h264_pay::output_packet_kind() const
{
    return packet_kind_e::SOCK;
}

int rtp_h264_pay::open()
{
    std::lock_guard<std::mutex> lock(mu);
    if (opened)
    {
        return 0;
    }
    rtp_h264_config cfg;
    cfg.mtu = mtu;
    cfg.payload_type = pt;
    cfg.ssrc = ssrc;
    cfg.fps = fps;
    packer = rtp_h264_packer(cfg);
    opened = true;
    return 0;
}

void rtp_h264_pay::close()
{
    std::lock_guard<std::mutex> lock(mu);
    pending.clear();
    packer.reset();
    opened = false;
}

int rtp_h264_pay::input(uint8_t port, const data_packet &in)
{
    if (0 != port)
    {
        return -EINVAL;
    }
    const frame_data &f = data_packet::cast<frame_data>(in);
    if (f.kind != media_kind_e::H264)
    {
        return -EINVAL;
    }

    std::lock_guard<std::mutex> lock(mu);
    if (packer.pack_annexb(f.buf.data, f.buf.size, f.pts, f.capture_mono_ns) < 0)
    {
        return -EINVAL;
    }
    pending_pts = f.pts;
    pending.clear();
    while (packer.pending())
    {
        std::vector<uint8_t> buf(1500);
        const int n = packer.pop_datagram(buf.data(), buf.size());
        if (n < 0)
        {
            break;
        }
        buf.resize(static_cast<size_t>(n));
        pending.push_back(std::move(buf));
    }
    return 0;
}

int rtp_h264_pay::output(uint8_t port, data_packet &out, int /*timeout_ms*/)
{
    if (0 != port)
    {
        return -EINVAL;
    }

    std::lock_guard<std::mutex> lock(mu);
    if (pending.empty())
    {
        return -EAGAIN;
    }

    const auto &front = pending.front();
    uint8_t    *buf = pool.acquire(front.size());
    if (nullptr == buf)
    {
        return -ENOMEM;
    }
    std::memcpy(buf, front.data(), front.size());
    auto sd = std::make_unique<sock_data>();
    sd->pts = pending_pts;
    sd->buf.reset(buf, front.size(), &packet_pool::release);
    out.reset(std::move(sd));
    pending.pop_front();
    return 0;
}

int rtp_h264_pay::configure(uint64_t /*key*/, int64_t /*value*/)
{
    return -ENOTSUP;
}

int rtp_h264_pay::query(uint64_t /*key*/, int64_t * /*value*/) const
{
    return -ENOTSUP;
}

int rtp_h264_pay::configure(std::string_view key, std::string_view *value)
{
    if (nullptr == value)
    {
        return -EINVAL;
    }

    std::lock_guard<std::mutex> lock(mu);
    if ("mtu" == key)
    {
        int64_t v = 0;
        char    buf[32];
        std::memcpy(buf, value->data(), value->size());
        buf[value->size()] = '\0';
        if (key_parse_i64(buf, &v) < 0 || v < 200 || v > 1500)
        {
            return -EINVAL;
        }
        mtu = static_cast<int>(v);
        return 0;
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
        fps = static_cast<int>(v);
        return 0;
    }
    return -ENOTSUP;
}

int rtp_h264_pay::query(std::string_view /*key*/, std::string_view * /*value*/) const
{
    return -ENOTSUP;
}

}  // namespace vstreamer
