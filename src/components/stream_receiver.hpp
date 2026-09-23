#ifndef VSTREAMER_COMPONENTS_STREAM_RECEIVER_HPP
#define VSTREAMER_COMPONENTS_STREAM_RECEIVER_HPP

#include "components/components_config.hpp"
#ifndef ENABLE_STREAM_RECEIVER
#error "stream_receiver requires -DENABLE_STREAM_RECEIVER=ON"
#endif

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "core/component_source.hpp"
#include "core/data_packet.hpp"
#include "core/packet_pool.hpp"

namespace vstreamer
{

/* Pad 0 (source): SOCK out → rtp_h264_depay. */
class stream_receiver : public component_source
{
public:
    stream_receiver();
    ~stream_receiver() override;

    stream_receiver(const stream_receiver &) = delete;
    stream_receiver &operator=(const stream_receiver &) = delete;

    [[nodiscard]] std::string name() const override;
    [[nodiscard]] media_kind_e output_kind() const override;

    [[nodiscard]] packet_kind_e output_packet_kind(uint8_t port) const override;

    int  open() override;
    void close() override;

    int output(uint8_t port, data_packet &out, int timeout_ms) override;

    int configure(uint64_t key, int64_t value) override;
    int query(uint64_t key, int64_t *value) const override;

    int configure(std::string_view key, std::string_view *value) override;
    int query(std::string_view key, std::string_view *value) const override;

private:
    void recv_thread_main();
    void stop_recv_thread();

    mutable std::mutex mu;
    bool               opened = false;

    std::string listen_spec;
    int         recv_fd = -1;

    packet_pool pool;

    std::mutex              q_mu;
    std::condition_variable q_cv;
    std::deque<data_packet> payload_queue;
    static constexpr size_t k_queue_depth = 4096;

    uint64_t recv_pkts = 0;
    uint64_t recv_bytes = 0;
    uint64_t recv_dropped = 0;
    uint64_t recv_lost = 0;
    uint16_t last_rtp_seq = 0;
    bool     have_rtp_seq = false;

    std::thread       recv_thread;
    std::atomic<bool> recv_stop {false};

    mutable std::string query_buf;
};

}  // namespace vstreamer

#endif  // VSTREAMER_COMPONENTS_STREAM_RECEIVER_HPP
