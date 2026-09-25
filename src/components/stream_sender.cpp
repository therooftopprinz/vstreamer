#include "components/stream_sender.hpp"

#include "core/host_util.hpp"
#include "core/key_util.hpp"
#include "core/stream_air_limits.hpp"
#include "core/stream_header.hpp"

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#include <chrono>
#include <random>
#include <thread>
#include <vector>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace vstreamer
{
namespace
{

double now_sec()
{
    struct timespec ts {};
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

void update_kbps_window(double &t0, uint64_t &acc, float &kbps, size_t nbytes)
{
    const double t = now_sec();
    if (t0 <= 0.)
    {
        t0 = t;
    }
    acc += nbytes;
    const double dt = t - t0;
    if (dt >= 0.5)
    {
        kbps = static_cast<float>(acc * 8.0 / dt / 1000.0);
        t0 = t;
        acc = 0;
    }
}

}  // namespace

stream_sender::stream_sender() : pool(static_cast<size_t>(1500), k_queue_depth) {}

stream_sender::~stream_sender()
{
    close();
}

std::string stream_sender::name() const
{
    return "stream_sender";
}

media_kind_e stream_sender::input_kind() const
{
    return media_kind_e::UNKNOWN;
}

packet_kind_e stream_sender::input_packet_kind(uint8_t port) const
{
    if (0 != port)
    {
        return packet_kind_e::UNKNOWN;
    }
    return packet_kind_e::SOCK;
}

void stream_sender::set_receiver_counters(stream_receiver_counters counters)
{
    std::lock_guard<std::mutex> lock(mu);
    peer = counters;
}

void stream_sender::enqueue_wire_copy(const uint8_t *data, size_t len)
{
    if (nullptr == data || 0 == len)
    {
        return;
    }
    if (len > k_stream_payload_max)
    {
        std::lock_guard<std::mutex> lock(mu);
        dropped++;
        return;
    }
    const size_t wire_len = k_stream_header_len + len;
    data_packet copy;
    uint8_t    *buf = pool.acquire(wire_len);
    if (nullptr == buf)
    {
        std::lock_guard<std::mutex> lock(mu);
        dropped++;
        return;
    }
    /* stream_sequence is stamped in send_thread_main() so each egress datagram gets a
     * unique value even if a buffer is reused before send. */
    std::memset(buf, 0, k_stream_header_len);
    std::memcpy(buf + k_stream_header_len, data, len);
    auto sd = std::make_unique<sock_data>();
    sd->pts = 0;
    sd->buf.reset(buf, wire_len, &packet_pool::release);
    copy.reset(std::move(sd));

    bool evicted = false;
    {
        std::lock_guard<std::mutex> lock(q_mu);
        if (queue.size() >= k_queue_depth)
        {
            queue.pop_front();
            evicted = true;
        }
        queue.push_back(std::move(copy));
    }
    if (evicted)
    {
        std::lock_guard<std::mutex> lock(mu);
        dropped++;
    }
    q_cv.notify_one();
}

void stream_sender::enqueue_fec_air(std::vector<std::vector<uint8_t>> *air)
{
    if (nullptr == air)
    {
        return;
    }
    for (auto &pkt : *air)
    {
        enqueue_wire_copy(pkt.data(), pkt.size());
    }
}

void stream_sender::pace_wire_send(size_t bytes)
{
    const int cap_kbps = max_wire_kbps.load(std::memory_order_relaxed);
    if (cap_kbps <= 0)
    {
        return;
    }

    const double max_bps = static_cast<double>(cap_kbps) * 1000.0;
    const double max_bytes_per_sec = max_bps / 8.0;
    const double burst_bytes = max_bytes_per_sec * 0.25;

    while (!send_stop.load(std::memory_order_relaxed))
    {
        const double now = now_sec();
        if (pace_last_sec <= 0.)
        {
            pace_last_sec = now;
            pace_bucket_bytes = burst_bytes;
        }
        const double dt = now - pace_last_sec;
        pace_last_sec = now;
        pace_bucket_bytes += dt * max_bytes_per_sec;
        if (pace_bucket_bytes > burst_bytes)
        {
            pace_bucket_bytes = burst_bytes;
        }
        if (pace_bucket_bytes >= static_cast<double>(bytes))
        {
            pace_bucket_bytes -= static_cast<double>(bytes);
            return;
        }
        const double deficit = static_cast<double>(bytes) - pace_bucket_bytes;
        pace_bucket_bytes = 0.;
        const double sleep_s = deficit / max_bytes_per_sec;
        if (sleep_s > 0.)
        {
            std::this_thread::sleep_for(
                std::chrono::duration<double>(std::min(sleep_s, 0.05)));
        }
    }
}

void stream_sender::stop_send_thread()
{
    if (send_thread.joinable())
    {
        send_stop = true;
        q_cv.notify_all();
        send_thread.join();
        send_stop = false;
    }
}

void stream_sender::send_thread_main()
{
    sockaddr_in dst {};
    bool        dst_ok = false;
    {
        std::lock_guard<std::mutex> lock(mu);
        dst_ok = have_dst;
        if (dst_ok)
        {
            std::memcpy(&dst, &dst_addr, sizeof(dst));
        }
    }

    while (!send_stop)
    {
        int wait_ms = 50;
        {
            std::vector<std::vector<uint8_t>> tick_air;
            {
                std::lock_guard<std::mutex> flock(fec_mu);
                fec.on_tick(&tick_air);
                if (fec.enabled())
                {
                    /* Poll often enough that the partial-block timeout is honoured. */
                    wait_ms = 5;
                }
            }
            if (!tick_air.empty())
            {
                enqueue_fec_air(&tick_air);
            }
        }

        data_packet pkt;
        {
            std::unique_lock<std::mutex> lock(q_mu);
            q_cv.wait_for(lock, std::chrono::milliseconds(wait_ms),
                          [this] { return send_stop || !queue.empty(); });
            if (send_stop)
            {
                break;
            }
            if (queue.empty())
            {
                continue;
            }
            pkt = std::move(queue.front());
            queue.pop_front();
        }

        if (!dst_ok || send_fd < 0)
        {
            continue;
        }

        {
            std::lock_guard<std::mutex> gate(gate_mu);
            if (!send_enabled)
            {
                continue;
            }
            if (deadline_sec > 0 && now_sec() >= deadline_sec)
            {
                send_enabled = false;
                deadline_sec = 0;
                continue;
            }
        }

        const sock_data &sd = data_packet::cast<sock_data>(pkt);
        bool stamp_stream_header = false;
        {
            std::lock_guard<std::mutex> lock(mu);
            stamp_stream_header =
                fec_block && sd.buf.size >= k_stream_header_len + rs_block_erasure::k_header_len;
        }
        if (stamp_stream_header)
        {
            const uint16_t seq = stream_sequence.fetch_add(1, std::memory_order_relaxed);
            stream_header_store_be16(sd.buf.data, seq);
        }
        pace_wire_send(sd.buf.size);
        const ssize_t n = sendto(send_fd, sd.buf.data, sd.buf.size, 0,
                                    reinterpret_cast<sockaddr *>(&dst), sizeof(dst));
        if (n < 0)
        {
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(mu);
            pkts_sent++;
            bytes_sent += static_cast<uint64_t>(n);
        }
    }
}

int stream_sender::open()
{
    std::lock_guard<std::mutex> lock(mu);
    if (opened)
    {
        return 0;
    }

    if (stream_spec.empty())
    {
        return -EINVAL;
    }

    char host[128];
    int  port = 0;
    if (parse_host_port(stream_spec, host, sizeof(host), &port) < 0)
    {
        return -EINVAL;
    }

    send_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (send_fd < 0)
    {
        return -errno;
    }

    constexpr int k_sock_buf = 16 * 1024 * 1024;
    (void)setsockopt(send_fd, SOL_SOCKET, SO_SNDBUF, &k_sock_buf, sizeof(k_sock_buf));

    sockaddr_in dst_in {};
    dst_in.sin_family = AF_INET;
    dst_in.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, host, &dst_in.sin_addr) != 1)
    {
        close();
        return -EINVAL;
    }

    std::memcpy(&dst_addr, &dst_in, sizeof(dst_in));
    have_dst = true;

    if (fec_block)
    {
        std::lock_guard<std::mutex> flock(fec_mu);
        if (!fec.init(fec_k, fec_n, fec_timeout_ms))
        {
            ::close(send_fd);
            send_fd = -1;
            have_dst = false;
            return -EINVAL;
        }
        fec_oversized = 0;
        std::fprintf(stderr,
                     "stream_sender: fec RS_BLOCK_ERASURE k=%d n=%d timeout=%d ms (%s)\n", fec_k,
                     fec_n, fec_timeout_ms, fec.impl_name());
    }

    opened = true;

    {
        std::random_device rd;
        stream_sequence.store(static_cast<uint16_t>(rd()), std::memory_order_relaxed);
    }

    send_stop = false;
    send_thread = std::thread(&stream_sender::send_thread_main, this);

    std::fprintf(stderr, "stream_sender: udp://%s:%d mtu=%d\n", host, port, mtu);
    return 0;
}

void stream_sender::close()
{
    disable_fec();
    stop_send_thread();

    std::lock_guard<std::mutex> lock(mu);
    if (send_fd >= 0)
    {
        ::close(send_fd);
        send_fd = -1;
    }
    have_dst = false;
    opened = false;

    {
        std::lock_guard<std::mutex> qlock(q_mu);
        while (!queue.empty())
        {
            queue.pop_front();
        }
    }
    q_cv.notify_all();
}

int stream_sender::input(uint8_t port, const data_packet &in)
{
    if (0 != port)
    {
        return -EINVAL;
    }
    const sock_data &src = data_packet::cast<sock_data>(in);
    const size_t     ingress_bytes = src.buf.size;

    bool fec_path = false;
    bool oversized = false;
    {
        std::vector<std::vector<uint8_t>> air;
        {
            std::lock_guard<std::mutex> flock(fec_mu);
            if (fec.enabled())
            {
                fec_path = true;
                const uint64_t before = fec.oversized();
                fec.push_app(src.buf.data, src.buf.size, &air);
                oversized = fec.oversized() != before;
            }
        }
        if (!air.empty())
        {
            enqueue_fec_air(&air);
        }
    }
    if (fec_path)
    {
        std::lock_guard<std::mutex> lock(mu);
        if (oversized)
        {
            fec_oversized++;
            dropped++;
        }
        update_kbps_window(ingress_rate_t0, ingress_rate_bytes, ingress_kbps, ingress_bytes);
        return 0;
    }

    data_packet      copy;
    uint8_t         *buf = pool.acquire(src.buf.size);
    if (nullptr == buf)
    {
        std::lock_guard<std::mutex> lock(mu);
        dropped++;
        return 0;
    }
    std::memcpy(buf, src.buf.data, src.buf.size);
    auto sd = std::make_unique<sock_data>();
    sd->pts = src.pts;
    sd->buf.reset(buf, src.buf.size, &packet_pool::release);
    copy.reset(std::move(sd));

    bool evicted = false;
    {
        std::lock_guard<std::mutex> lock(q_mu);
        if (queue.size() >= k_queue_depth)
        {
            queue.pop_front();
            evicted = true;
        }
        queue.push_back(std::move(copy));
    }
    {
        std::lock_guard<std::mutex> lock(mu);
        if (evicted)
        {
            dropped++;
        }
        update_kbps_window(ingress_rate_t0, ingress_rate_bytes, ingress_kbps, ingress_bytes);
    }
    q_cv.notify_one();
    return 0;
}

int stream_sender::set_enabled(bool on, int timeout_ms)
{
    std::lock_guard<std::mutex> gate(gate_mu);
    send_enabled = on;
    if (!on)
    {
        deadline_sec = 0;
        return 0;
    }
    if (timeout_ms <= 0)
    {
        deadline_sec = 0;
    }
    else
    {
        deadline_sec = now_sec() + (timeout_ms / 1000.0);
    }
    return 0;
}

bool stream_sender::enabled() const
{
    std::lock_guard<std::mutex> gate(gate_mu);
    if (!send_enabled)
    {
        return false;
    }
    if (deadline_sec > 0 && now_sec() >= deadline_sec)
    {
        return false;
    }
    return true;
}

int stream_sender::reinit_fec_if_active()
{
    int k = 0;
    int n = 0;
    int timeout_ms = 0;
    {
        std::lock_guard<std::mutex> lock(mu);
        if (!opened || !fec_block)
        {
            return 0;
        }
        k = fec_k;
        n = fec_n;
        timeout_ms = fec_timeout_ms;
    }
    if (k > n)
    {
        return -EINVAL;
    }

    /* Flush the partial block under the old k/n so no app packet is lost;
     * block_id keeps counting so the receiver's dedupe/order state stays valid. */
    std::vector<std::vector<uint8_t>> air;
    bool                              ok = false;
    {
        std::lock_guard<std::mutex> flock(fec_mu);
        fec.flush(&air);
        ok = fec.init(k, n, timeout_ms);
    }
    enqueue_fec_air(&air);
    if (!ok)
    {
        return -EINVAL;
    }
    std::fprintf(stderr, "stream_sender: fec RS_BLOCK_ERASURE k=%d n=%d timeout=%d ms\n", k, n,
                 timeout_ms);
    return 0;
}

void stream_sender::disable_fec()
{
    std::vector<std::vector<uint8_t>> air;
    {
        std::lock_guard<std::mutex> flock(fec_mu);
        if (!fec.enabled())
        {
            return;
        }
        fec.flush(&air);
        fec.disable();
    }
    enqueue_fec_air(&air);
}

int stream_sender::configure(uint64_t /*key*/, int64_t /*value*/)
{
    return -ENOTSUP;
}

int stream_sender::query(uint64_t /*key*/, int64_t * /*value*/) const
{
    return -ENOTSUP;
}

int stream_sender::configure(std::string_view key, std::string_view *value)
{
    if (nullptr == value)
    {
        return -EINVAL;
    }

    if ("stream" == key)
    {
        std::lock_guard<std::mutex> lock(mu);
        stream_spec.assign(value->data(), value->size());
        return 0;
    }
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
    if ("max_kbps" == key)
    {
        int64_t v = 0;
        char    buf[32];
        std::memcpy(buf, value->data(), value->size());
        buf[value->size()] = '\0';
        if (key_parse_i64(buf, &v) < 0 || v < 0 || v > 500'000)
        {
            return -EINVAL;
        }
        max_wire_kbps.store(static_cast<int>(v), std::memory_order_relaxed);
        pace_bucket_bytes = 0.;
        pace_last_sec = 0.;
        return 0;
    }
    if ("fec" == key)
    {
        std::string mode(value->data(), value->size());
        if ("none" == mode || "NONE" == mode)
        {
            {
                std::lock_guard<std::mutex> lock(mu);
                fec_block = false;
            }
            disable_fec();
            return 0;
        }
        if ("block" == mode || "RS_BLOCK_ERASURE" == mode)
        {
            {
                std::lock_guard<std::mutex> lock(mu);
                fec_block = true;
            }
            return reinit_fec_if_active();
        }
        return -EINVAL;
    }
    if ("fec_k" == key)
    {
        int64_t v = 0;
        char    buf[32];
        std::memcpy(buf, value->data(), value->size());
        buf[value->size()] = '\0';
        if (key_parse_i64(buf, &v) < 0 || v < 1 || v > 254)
        {
            return -EINVAL;
        }
        int old_k = 0;
        {
            std::lock_guard<std::mutex> lock(mu);
            if (static_cast<int>(v) > fec_n)
            {
                return -EINVAL;
            }
            old_k = fec_k;
            fec_k = static_cast<int>(v);
            fec_block = true;
        }
        if (reinit_fec_if_active() < 0)
        {
            std::lock_guard<std::mutex> lock(mu);
            fec_k = old_k;
            return -EINVAL;
        }
        return 0;
    }
    if ("fec_n" == key)
    {
        int64_t v = 0;
        char    buf[32];
        std::memcpy(buf, value->data(), value->size());
        buf[value->size()] = '\0';
        if (key_parse_i64(buf, &v) < 0 || v < 1 || v > 255)
        {
            return -EINVAL;
        }
        int old_n = 0;
        {
            std::lock_guard<std::mutex> lock(mu);
            if (fec_k > static_cast<int>(v))
            {
                return -EINVAL;
            }
            old_n = fec_n;
            fec_n = static_cast<int>(v);
            fec_block = true;
        }
        if (reinit_fec_if_active() < 0)
        {
            std::lock_guard<std::mutex> lock(mu);
            fec_n = old_n;
            return -EINVAL;
        }
        return 0;
    }
    if ("fec_timeout_ms" == key)
    {
        int64_t v = 0;
        char    buf[32];
        std::memcpy(buf, value->data(), value->size());
        buf[value->size()] = '\0';
        if (key_parse_i64(buf, &v) < 0 || v < 0 || v > 60'000)
        {
            return -EINVAL;
        }
        {
            std::lock_guard<std::mutex> lock(mu);
            fec_timeout_ms = static_cast<int>(v);
        }
        return reinit_fec_if_active();
    }
    return -ENOTSUP;
}

int stream_sender::query(std::string_view key, std::string_view *value) const
{
    if (nullptr == value)
    {
        return -EINVAL;
    }

    if ("stream" == key)
    {
        std::lock_guard<std::mutex> lock(mu);
        query_buf = stream_spec;
        *value = query_buf;
        return 0;
    }
    if ("in_rate" == key)
    {
        std::lock_guard<std::mutex> lock(mu);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f", static_cast<double>(ingress_kbps));
        query_buf = buf;
        *value = query_buf;
        return 0;
    }
    if ("fec_oversized" == key)
    {
        std::lock_guard<std::mutex> lock(mu);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%" PRIu64, fec_oversized);
        query_buf = buf;
        *value = query_buf;
        return 0;
    }
    if ("fec_k" == key || "fec_n" == key)
    {
        std::lock_guard<std::mutex> lock(mu);
        const int v = ("fec_k" == key) ? fec_k : fec_n;
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d", v);
        query_buf = buf;
        *value = query_buf;
        return 0;
    }
    if ("stats" == key)
    {
        std::lock_guard<std::mutex> lock(mu);
        char buf[320];
        if (fec_block)
        {
            std::snprintf(buf, sizeof(buf),
                          "pkts=%" PRIu64 " bytes=%" PRIu64 " dropped=%" PRIu64
                          " in_rate=%.1f fec_oversized=%" PRIu64 " fec=k=%d,n=%d",
                          pkts_sent, bytes_sent, dropped, static_cast<double>(ingress_kbps),
                          fec_oversized, fec_k, fec_n);
        }
        else
        {
            std::snprintf(buf, sizeof(buf),
                          "pkts=%" PRIu64 " bytes=%" PRIu64 " dropped=%" PRIu64
                          " in_rate=%.1f",
                          pkts_sent, bytes_sent, dropped, static_cast<double>(ingress_kbps));
        }
        query_buf = buf;
        *value = query_buf;
        return 0;
    }
    if ("peer_udp_packet_received" == key || "peer_fec_packet_received" == key ||
        "peer_udp_gap_count" == key || "peer_fec_gap_count" == key ||
        "peer_fec_air_shard_received" == key)
    {
        std::lock_guard<std::mutex> lock(mu);
        uint64_t n = 0;
        if ("peer_udp_packet_received" == key)
        {
            n = peer.udp_packet_received;
        }
        else if ("peer_fec_packet_received" == key)
        {
            n = peer.fec_packet_received;
        }
        else if ("peer_udp_gap_count" == key)
        {
            n = peer.udp_gap_count;
        }
        else if ("peer_fec_gap_count" == key)
        {
            n = peer.fec_gap_count;
        }
        else
        {
            n = peer.fec_air_shard_received;
        }
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%" PRIu64, n);
        query_buf = buf;
        *value = query_buf;
        return 0;
    }
    if ("peer_loss_udp_pct" == key || "peer_loss_fec_pct" == key)
    {
        std::lock_guard<std::mutex> lock(mu);
        char buf[32];
        const double v = ("peer_loss_udp_pct" == key) ? peer.loss_udp_pct : peer.loss_fec_pct;
        std::snprintf(buf, sizeof(buf), "%.4f", v);
        query_buf = buf;
        *value = query_buf;
        return 0;
    }
    if ("telemetry" == key)
    {
        std::lock_guard<std::mutex> lock(mu);
        char buf[320];
        std::snprintf(buf, sizeof(buf),
                      "peer_udp=%" PRIu64 " peer_fec=%" PRIu64
                      " udp_gap=%" PRIu64 " fec_gap=%" PRIu64
                      " peer_loss_udp_pct=%.2f peer_loss_fec_pct=%.2f",
                      peer.udp_packet_received, peer.fec_packet_received, peer.udp_gap_count,
                      peer.fec_gap_count, peer.loss_udp_pct, peer.loss_fec_pct);
        query_buf = buf;
        *value = query_buf;
        return 0;
    }
    return -ENOTSUP;
}

}  // namespace vstreamer
