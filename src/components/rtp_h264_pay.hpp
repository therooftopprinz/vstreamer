#ifndef VSTREAMER_COMPONENTS_RTP_H264_PAY_HPP
#define VSTREAMER_COMPONENTS_RTP_H264_PAY_HPP

#include "components/components_config.hpp"
#ifndef ENABLE_RTP_H264_PAY
#error "rtp_h264_pay requires -DENABLE_RTP_H264_PAY=ON"
#endif

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "core/component_coder.hpp"
#include "core/data_packet.hpp"
#include "core/packet_pool.hpp"
#include "core/rtp_h264.hpp"

namespace vstreamer
{

class rtp_h264_pay : public component_coder
{
public:
    rtp_h264_pay();
    ~rtp_h264_pay() override;

    rtp_h264_pay(const rtp_h264_pay &) = delete;
    rtp_h264_pay &operator=(const rtp_h264_pay &) = delete;

    [[nodiscard]] std::string name() const override;
    [[nodiscard]] media_kind_e input_kind() const override;
    [[nodiscard]] media_kind_e output_kind() const override;

    [[nodiscard]] packet_kind_e input_packet_kind() const override;
    [[nodiscard]] packet_kind_e output_packet_kind() const override;

    int  open() override;
    void close() override;

    int input(uint8_t port, const data_packet &in) override;
    int output(uint8_t port, data_packet &out, int timeout_ms) override;

    int configure(uint64_t key, int64_t value) override;
    int query(uint64_t key, int64_t *value) const override;

    int configure(std::string_view key, std::string_view *value) override;
    int query(std::string_view key, std::string_view *value) const override;

private:
    mutable std::mutex mu;
    bool               opened = false;

    int         mtu = 1400;
    int         fps = 30;
    uint8_t     pt = 96;
    uint32_t    ssrc = 0xC0DE0001u;
    rtp_h264_packer packer {rtp_h264_config {}};

    std::deque<std::vector<uint8_t>> pending;
    int64_t                          pending_pts = 0;

    packet_pool pool;

    mutable std::string query_buf;
};

}  // namespace vstreamer

#endif  // VSTREAMER_COMPONENTS_RTP_H264_PAY_HPP
