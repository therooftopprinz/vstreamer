#ifndef VSTREAMER_CORE_RTP_H264_HPP
#define VSTREAMER_CORE_RTP_H264_HPP

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace vstreamer
{

struct rtp_h264_config
{
    int      mtu = 1400;
    uint8_t  payload_type = 96;
    uint32_t ssrc = 0xC0DE0001u;
    int      fps = 30;
};

/* Annex-B H.264 access unit → complete RTP/UDP datagrams (12-byte RTP header + payload). */
class rtp_h264_packer
{
public:
    explicit rtp_h264_packer(rtp_h264_config cfg);

    void reset();

    /* Clears pending and enqueues new datagrams. */
    int pack_annexb(const uint8_t *data, size_t size, int64_t pts, int64_t capture_mono_ns = 0);

    bool pending() const { return !queue.empty(); }

    /* Copies one datagram into dst; returns size or negative errno. */
    int pop_datagram(uint8_t *dst, size_t cap);

private:
    int append_datagram(const uint8_t *payload, int plen, int marker, uint32_t ts,
                        int64_t capture_mono_ns = 0);
    int send_nal(const uint8_t *nal, int len, int marker, uint32_t ts,
                 int64_t capture_mono_ns = 0);
    void cache_param(const uint8_t *nal, int len);

    rtp_h264_config cfg;
    uint16_t        seq = 0;
    uint8_t         sps[256];
    int             sps_len = 0;
    uint8_t         pps[256];
    int             pps_len = 0;
    std::deque<std::vector<uint8_t>> queue;
};

/* One RTP datagram in → depay state; complete AU out as Annex-B. */
class rtp_h264_depacketizer
{
public:
    explicit rtp_h264_depacketizer(int fps_in = 30) : fps(fps_in > 0 ? fps_in : 30) {}

    void reset();

    /* Returns 0 when no AU ready; 1 when au_out filled; negative on error. */
    int feed(const uint8_t *datagram, size_t len, std::vector<uint8_t> *au_out);

    [[nodiscard]] float packet_loss() const { return loss; }

    /* Frame index for the last completed AU (from RTP timestamp). */
    [[nodiscard]] int64_t au_pts() const { return last_au_frame_pts; }

    [[nodiscard]] int64_t au_capture_mono_ns() const { return last_au_capture_mono_ns; }

private:
    void note_au_timestamp(const uint8_t *datagram, size_t len);

    int                 fps = 30;
    int64_t             last_au_frame_pts = 0;
    int64_t             last_au_capture_mono_ns = 0;
    std::vector<uint8_t> fu_buf;
    bool                 fu_active = false;
    uint8_t              fu_type = 0;
    uint16_t             last_seq = 0;
    bool                 have_seq = false;
    uint64_t             expected = 0;
    uint64_t             received = 0;
    float                loss = 0.f;
};

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_RTP_H264_HPP
