#ifndef VSTREAMER_COMPONENTS_STREAM_SENDER_HPP
#define VSTREAMER_COMPONENTS_STREAM_SENDER_HPP

#include "components/components_config.hpp"
#ifndef ENABLE_STREAM_SENDER
#error "stream_sender requires -DENABLE_STREAM_SENDER=ON"
#endif

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include <netinet/in.h>

#include "core/stream_telemetry.hpp"
#include "core/component_sink.hpp"
#include "core/data_packet.hpp"
#include "core/packet_pool.hpp"
#include "core/rs_block_erasure.hpp"

namespace vstreamer
{

/* Pad 0 (sink): SOCK in from rtp_h264_pay → UDP egress. Link metrics via query(). */
class stream_sender : public component_sink
{
public:
    stream_sender();
    ~stream_sender() override;

    stream_sender(const stream_sender &) = delete;
    stream_sender &operator=(const stream_sender &) = delete;

    [[nodiscard]] std::string name() const override;
    [[nodiscard]] media_kind_e input_kind() const override;

    [[nodiscard]] uint8_t input_pad_count() const override { return 1; }

    [[nodiscard]] packet_kind_e input_packet_kind(uint8_t port) const override;

    int  open() override;
    void close() override;

    int input(uint8_t port, const data_packet &in) override;

    int set_enabled(bool on, int timeout_ms) override;
    [[nodiscard]] bool enabled() const override;

    void set_receiver_counters(stream_receiver_counters counters);

    int configure(uint64_t key, int64_t value) override;
    int query(uint64_t key, int64_t *value) const override;

    int configure(std::string_view key, std::string_view *value) override;
    int query(std::string_view key, std::string_view *value) const override;

private:
    void send_thread_main();
    void stop_send_thread();
    void pace_wire_send(size_t bytes);
    void enqueue_wire_copy(const uint8_t *data, size_t len);
    void enqueue_fec_air(std::vector<std::vector<uint8_t>> *air);
    /* Flush partial block, (re)init with current k/n/timeout. No locks held. */
    int  reinit_fec_if_active();
    /* Flush partial block and stop encoding. No locks held. */
    void disable_fec();

    mutable std::mutex mu;
    bool               opened = false;

    std::string stream_spec;
    int         mtu = 1400;

    int         send_fd = -1;
    sockaddr_in dst_addr {};
    bool        have_dst = false;

    bool              send_enabled = false;
    double            deadline_sec = 0;
    mutable std::mutex gate_mu;

    stream_receiver_counters peer {};

    packet_pool pool;

    std::mutex              q_mu;
    std::condition_variable q_cv;
    std::deque<data_packet> queue;
    static constexpr size_t k_queue_depth = 4096;

    std::thread       send_thread;
    std::atomic<bool> send_stop {false};

    uint64_t pkts_sent = 0;
    uint64_t bytes_sent = 0;
    uint64_t dropped = 0;

    double   ingress_rate_t0 = 0.;
    uint64_t ingress_rate_bytes = 0;
    float    ingress_kbps = 0.f;

    std::atomic<int> max_wire_kbps {0};
    double           pace_bucket_bytes = 0.;
    double           pace_last_sec = 0.;

    mutable std::string query_buf;

    /* Guards fec (touched by input(), the send thread and configure()).
     * Lock order: mu -> fec_mu; never hold fec_mu while taking mu/q_mu. */
    mutable std::mutex fec_mu;
    rs_block_erasure   fec;
    bool              fec_block = false;
    int               fec_k = 10;
    int               fec_n = 12;
    int               fec_timeout_ms = rs_block_erasure::k_default_timeout_ms;
    uint64_t          fec_oversized = 0;

    std::atomic<uint16_t> stream_sequence {0};
};

}  // namespace vstreamer

#endif  // VSTREAMER_COMPONENTS_STREAM_SENDER_HPP
