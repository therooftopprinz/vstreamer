#include "core/rtp_h264.hpp"

#include <cerrno>
#include <cstring>
#include <ctime>
#include <unistd.h>

namespace vstreamer
{
namespace
{

constexpr int k_rtp_hdr = 12;
constexpr int k_max_rtp = 1500;
constexpr int k_max_param = 256;
constexpr int k_capture_ext_words = 3;
constexpr int k_capture_ext_total = 4 + k_capture_ext_words * 4;
constexpr uint8_t k_capture_ext_id_len = 0x17;

uint32_t pts_to_rtp_ts(int64_t pts, int fps)
{
    const int f = fps > 0 ? fps : 30;
    return static_cast<uint32_t>((pts * 90000) / f);
}

}  // namespace

rtp_h264_packer::rtp_h264_packer(rtp_h264_config cfg_in) : cfg(cfg_in)
{
    seq = static_cast<uint16_t>(getpid() ^ static_cast<unsigned>(time(nullptr)));
}

void rtp_h264_packer::reset()
{
    queue.clear();
    sps_len = 0;
    pps_len = 0;
}

int rtp_h264_packer::append_datagram(const uint8_t *payload, int plen, int marker, uint32_t ts,
                                     int64_t capture_mono_ns)
{
    const bool     have_capture = capture_mono_ns > 0;
    const int      hdr_extra = have_capture ? k_capture_ext_total : 0;
    const int      total = k_rtp_hdr + hdr_extra + plen;
    if (plen < 0 || total > cfg.mtu || total > k_max_rtp)
    {
        return -EINVAL;
    }

    std::vector<uint8_t> pkt(static_cast<size_t>(total));
    pkt[0] = static_cast<uint8_t>(have_capture ? 0x90 : 0x80);
    pkt[1] = static_cast<uint8_t>(cfg.payload_type | (marker ? 0x80 : 0));
    pkt[2] = static_cast<uint8_t>(seq >> 8);
    pkt[3] = static_cast<uint8_t>(seq & 0xff);
    pkt[4] = static_cast<uint8_t>(ts >> 24);
    pkt[5] = static_cast<uint8_t>(ts >> 16);
    pkt[6] = static_cast<uint8_t>(ts >> 8);
    pkt[7] = static_cast<uint8_t>(ts);
    pkt[8] = static_cast<uint8_t>(cfg.ssrc >> 24);
    pkt[9] = static_cast<uint8_t>(cfg.ssrc >> 16);
    pkt[10] = static_cast<uint8_t>(cfg.ssrc >> 8);
    pkt[11] = static_cast<uint8_t>(cfg.ssrc);
    if (have_capture)
    {
        pkt[12] = 0xbe;
        pkt[13] = 0xde;
        pkt[14] = 0;
        pkt[15] = static_cast<uint8_t>(k_capture_ext_words);
        pkt[16] = k_capture_ext_id_len;
        std::memcpy(pkt.data() + 17, &capture_mono_ns, sizeof(capture_mono_ns));
        pkt[25] = 0;
        pkt[26] = 0;
        pkt[27] = 0;
    }
    std::memcpy(pkt.data() + k_rtp_hdr + hdr_extra, payload, static_cast<size_t>(plen));
    seq++;
    queue.push_back(std::move(pkt));
    return 0;
}

int rtp_h264_packer::send_nal(const uint8_t *nal, int len, int marker, uint32_t ts,
                              int64_t capture_mono_ns)
{
    const bool have_capture = capture_mono_ns > 0;
    const int  hdr_extra = have_capture ? k_capture_ext_total : 0;
    const int  max_single = cfg.mtu - k_rtp_hdr - hdr_extra;
    if (len <= 0)
    {
        return 0;
    }
    if (len <= max_single)
    {
        return append_datagram(nal, len, marker, ts, capture_mono_ns);
    }

    const int max_fu = cfg.mtu - k_rtp_hdr - hdr_extra - 2;
    if (max_fu < 1)
    {
        return -EINVAL;
    }

    const uint8_t type = nal[0] & 0x1f;
    const uint8_t nri = nal[0] & 0x60;
    const uint8_t *p = nal + 1;
    int            left = len - 1;
    int            first = 1;
    while (left > 0)
    {
        const int chunk = left < max_fu ? left : max_fu;
        const int last = (left - chunk) == 0;
        uint8_t   fu[k_max_rtp];
        fu[0] = static_cast<uint8_t>(nri | 28);
        fu[1] = static_cast<uint8_t>(type | (first ? 0x80 : 0) | (last ? 0x40 : 0));
        std::memcpy(fu + 2, p, static_cast<size_t>(chunk));
        if (append_datagram(fu, chunk + 2, marker && last, ts, capture_mono_ns) < 0)
        {
            return -EINVAL;
        }
        p += chunk;
        left -= chunk;
        first = 0;
    }
    return 0;
}

void rtp_h264_packer::cache_param(const uint8_t *nal, int len)
{
    const int type = nal[0] & 0x1f;
    if (len <= 0 || len > k_max_param)
    {
        return;
    }
    if (7 == type)
    {
        std::memcpy(sps, nal, static_cast<size_t>(len));
        sps_len = len;
    }
    else if (8 == type)
    {
        std::memcpy(pps, nal, static_cast<size_t>(len));
        pps_len = len;
    }
}

int rtp_h264_packer::pack_annexb(const uint8_t *data, size_t size, int64_t pts,
                                 int64_t capture_mono_ns)
{
    queue.clear();
    const uint32_t ts = pts_to_rtp_ts(pts, cfg.fps);

    const uint8_t *nal_ptr[64];
    int            nal_len[64];
    int            nn = 0;
    size_t         i = 0;
    while (i + 3 < size && nn < 64)
    {
        int sc = 0;
        if (i + 3 < size && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
        {
            sc = 3;
        }
        else if (i + 4 <= size && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 &&
                 data[i + 3] == 1)
        {
            sc = 4;
        }
        else
        {
            ++i;
            continue;
        }
        const int start = static_cast<int>(i + sc);
        int       j = start;
        while (j + 3 < static_cast<int>(size))
        {
            if (data[j] == 0 && data[j + 1] == 0 &&
                (data[j + 2] == 1 || (data[j + 2] == 0 && j + 3 < static_cast<int>(size) &&
                                      data[j + 3] == 1)))
            {
                break;
            }
            ++j;
        }
        if (j + 3 >= static_cast<int>(size))
        {
            j = static_cast<int>(size);
        }
        const int len = j - start;
        if (len > 0)
        {
            const int type = data[start] & 0x1f;
            cache_param(data + start, len);
            if (9 != type)
            {
                nal_ptr[nn] = data + start;
                nal_len[nn] = len;
                ++nn;
            }
        }
        i = j;
    }

    if (0 == nn && size > 0)
    {
        int off = 0;
        if (size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] > 0 &&
            data[3] < static_cast<uint8_t>(size))
        {
            off = 4;
        }
        nal_ptr[0] = data + off;
        nal_len[0] = static_cast<int>(size) - off;
        nn = 1;
    }

    int sent_sps = 0;
    for (int n = 0; n < nn; ++n)
    {
        const int type = nal_ptr[n][0] & 0x1f;
        const int is_slice = (type >= 1 && type <= 5);
        if (is_slice && !sent_sps && sps_len > 0 && pps_len > 0)
        {
            if (send_nal(sps, sps_len, 0, ts, capture_mono_ns) < 0)
            {
                return -EINVAL;
            }
            if (send_nal(pps, pps_len, 0, ts, capture_mono_ns) < 0)
            {
                return -EINVAL;
            }
            sent_sps = 1;
        }
        int marker = (n == nn - 1);
        if (7 == type || 8 == type)
        {
            marker = 0;
        }
        if (send_nal(nal_ptr[n], nal_len[n], marker, ts, capture_mono_ns) < 0)
        {
            return -EINVAL;
        }
    }
    return 0;
}

int rtp_h264_packer::pop_datagram(uint8_t *dst, size_t cap)
{
    if (queue.empty())
    {
        return -EAGAIN;
    }
    const auto &front = queue.front();
    if (front.size() > cap)
    {
        return -ENOMEM;
    }
    std::memcpy(dst, front.data(), front.size());
    const int n = static_cast<int>(front.size());
    queue.pop_front();
    return n;
}

void rtp_h264_depacketizer::reset()
{
    fu_buf.clear();
    fu_active = false;
    fu_type = 0;
    last_seq = 0;
    have_seq = false;
    expected = 0;
    received = 0;
    loss = 0.f;
    last_au_frame_pts = 0;
    last_au_capture_mono_ns = 0;
}

[[nodiscard]] int rtp_datagram_payload_offset(const uint8_t *datagram, size_t len,
                                              int64_t *capture_mono_ns)
{
    if (nullptr != capture_mono_ns)
    {
        *capture_mono_ns = 0;
    }
    if (len < k_rtp_hdr)
    {
        return -EINVAL;
    }
    int off = k_rtp_hdr;
    if ((datagram[0] & 0x10) == 0)
    {
        return off;
    }
    if (len < static_cast<size_t>(off + 4))
    {
        return -EINVAL;
    }
    const uint16_t profile =
        static_cast<uint16_t>((static_cast<uint16_t>(datagram[12]) << 8) | datagram[13]);
    if (profile != 0xbede)
    {
        return off;
    }
    const uint16_t ext_words =
        static_cast<uint16_t>((static_cast<uint16_t>(datagram[14]) << 8) | datagram[15]);
    const int ext_total = 4 + static_cast<int>(ext_words) * 4;
    if (len < static_cast<size_t>(off + ext_total))
    {
        return -EINVAL;
    }
    if (nullptr != capture_mono_ns && ext_words >= k_capture_ext_words &&
        datagram[16] == k_capture_ext_id_len && len >= static_cast<size_t>(off + 12))
    {
        std::memcpy(capture_mono_ns, datagram + 17, sizeof(int64_t));
    }
    return off + ext_total;
}

void rtp_h264_depacketizer::note_au_timestamp(const uint8_t *datagram, size_t len)
{
    if (nullptr == datagram || len < k_rtp_hdr)
    {
        return;
    }
    const uint32_t rtp_ts = (static_cast<uint32_t>(datagram[4]) << 24) |
                            (static_cast<uint32_t>(datagram[5]) << 16) |
                            (static_cast<uint32_t>(datagram[6]) << 8) |
                            static_cast<uint32_t>(datagram[7]);
    last_au_frame_pts = (static_cast<int64_t>(rtp_ts) * fps) / 90000;
    int64_t cap = 0;
    (void)rtp_datagram_payload_offset(datagram, len, &cap);
    last_au_capture_mono_ns = cap;
}

int rtp_h264_depacketizer::feed(const uint8_t *datagram, size_t len, std::vector<uint8_t> *au_out)
{
    if (nullptr == datagram || nullptr == au_out || len < k_rtp_hdr)
    {
        return -EINVAL;
    }

    const uint16_t seq = static_cast<uint16_t>((datagram[2] << 8) | datagram[3]);
    if (have_seq)
    {
        const uint16_t next = static_cast<uint16_t>(last_seq + 1);
        if (seq != next)
        {
            const uint16_t gap = static_cast<uint16_t>(seq - next);
            expected += gap;
        }
    }
    else
    {
        have_seq = true;
    }
    last_seq = seq;
    ++received;

    if (expected > 0)
    {
        loss = static_cast<float>(expected) / static_cast<float>(received + expected);
    }

    int payload_off = rtp_datagram_payload_offset(datagram, len, nullptr);
    if (payload_off < k_rtp_hdr)
    {
        return -EINVAL;
    }
    const uint8_t *payload = datagram + payload_off;
    const size_t   plen = len - static_cast<size_t>(payload_off);
    if (0 == plen)
    {
        return 0;
    }

    const uint8_t nal_hdr = payload[0];
    const uint8_t type = nal_hdr & 0x1f;

    if (type >= 1 && type <= 23)
    {
        au_out->clear();
        au_out->push_back(0);
        au_out->push_back(0);
        au_out->push_back(0);
        au_out->push_back(1);
        au_out->insert(au_out->end(), payload, payload + plen);
        note_au_timestamp(datagram, len);
        return 1;
    }

    if (28 != type)
    {
        return 0;
    }

    if (plen < 2)
    {
        return -EINVAL;
    }

    const uint8_t fu_hdr = payload[1];
    const bool    start = (fu_hdr & 0x80) != 0;
    const bool    end = (fu_hdr & 0x40) != 0;
    const uint8_t nal_type = fu_hdr & 0x1f;

    if (start)
    {
        fu_buf.clear();
        fu_buf.push_back(static_cast<uint8_t>((nal_hdr & 0xe0) | nal_type));
        fu_buf.insert(fu_buf.end(), payload + 2, payload + plen);
        fu_active = true;
        fu_type = nal_type;
    }
    else if (fu_active)
    {
        fu_buf.insert(fu_buf.end(), payload + 2, payload + plen);
    }

    if (end && fu_active)
    {
        au_out->clear();
        au_out->push_back(0);
        au_out->push_back(0);
        au_out->push_back(0);
        au_out->push_back(1);
        au_out->insert(au_out->end(), fu_buf.begin(), fu_buf.end());
        fu_active = false;
        note_au_timestamp(datagram, len);
        return 1;
    }

    return 0;
}

}  // namespace vstreamer
