#include "components/stream_receiver.hpp"

#include "core/fec_stream_header.hpp"
#include "core/host_util.hpp"
#include "core/key_util.hpp"
#include "core/sequence_gap.hpp"
#include "core/stream_air_limits.hpp"
#include "core/stream_header.hpp"

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#include <chrono>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

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

void note_fec_output_gaps(vstreamer::rs_block_erasure &fec, uint64_t &fec_gap_count)
{
    fec_gap_count += fec.take_fail_lost_app_pkts();
    (void)fec.take_fail_missing_shards();
    (void)fec.take_decode_fail();
}

}  // namespace

namespace vstreamer
{

stream_receiver::stream_receiver() : pool(static_cast<size_t>(1500), k_queue_depth) {}

stream_receiver::~stream_receiver()
{
    close();
}

std::string stream_receiver::name() const
{
    return "stream_receiver";
}

media_kind_e stream_receiver::output_kind() const
{
    return media_kind_e::UNKNOWN;
}

packet_kind_e stream_receiver::output_packet_kind(uint8_t port) const
{
    if (0 != port)
    {
        return packet_kind_e::UNKNOWN;
    }
    return packet_kind_e::SOCK;
}

void stream_receiver::enqueue_payload_copy(const uint8_t *data, size_t len)
{
    if (nullptr == data || 0 == len || len + k_fec_stream_header_len > k_stream_payload_max)
    {
        std::lock_guard<std::mutex> lock(mu);
        recv_dropped++;
        return;
    }

    data_packet pkt;
    const size_t wire_len = k_fec_stream_header_len + len;
    uint8_t     *copy = pool.acquire(wire_len);
    if (nullptr == copy)
    {
        std::lock_guard<std::mutex> lock(mu);
        recv_dropped++;
        return;
    }

    const uint16_t seq = fec_payload_sequence++;
    fec_stream_header_store_be16(copy, seq);
    std::memcpy(copy + k_fec_stream_header_len, data, len);
    auto sd = std::make_unique<sock_data>();
    sd->pts = 0;
    sd->buf.reset(copy, wire_len, &packet_pool::release);
    pkt.reset(std::move(sd));

    bool evicted = false;
    {
        std::lock_guard<std::mutex> lock(q_mu);
        if (payload_queue.size() >= k_queue_depth)
        {
            payload_queue.pop_front();
            evicted = true;
        }
        payload_queue.push_back(std::move(pkt));
    }
    {
        std::lock_guard<std::mutex> lock(mu);
        fec_packet_received++;
        recv_bytes += len;
        if (evicted)
        {
            recv_dropped++;
        }
    }
    q_cv.notify_all();
}

void stream_receiver::enqueue_payloads(std::vector<std::vector<uint8_t>> *payloads)
{
    if (nullptr == payloads)
    {
        return;
    }
    for (const auto &payload : *payloads)
    {
        enqueue_payload_copy(payload.data(), payload.size());
    }
    payloads->clear();
}

void stream_receiver::ingest_datagram(const uint8_t *data, size_t len)
{
    std::vector<std::vector<uint8_t>> payloads;
    {
        std::lock_guard<std::mutex> lock(mu);
        udp_packet_received++;
        recv_wire_bytes += len;

        /* stream_header_s always prefixes the FEC shard (UDP gap telemetry). */
        const uint8_t *fec_buf = data;
        size_t         fec_len = len;
        if (stream_datagram_len_ok(len))
        {
            const uint16_t seq = stream_header_sequence_be16(data);
            udp_gap_count +=
                note_u16_forward_gap(seq, last_udp_seq, have_udp_seq);
            const uint8_t *fec_ptr = stream_fec_shard(data, len, &fec_len);
            if (fec_ptr != nullptr)
            {
                fec_buf = fec_ptr;
            }
            if (fec_len >= rs_block_erasure::k_header_len)
            {
                fec_air_shard_received++;
            }
        }

        fec.push_air(fec_buf, fec_len, &payloads);
        note_fec_output_gaps(fec, fec_gap_count);
        fec_rec = fec.recovered();
        fec_lost = fec.decode_fail();
    }
    enqueue_payloads(&payloads);
}

void stream_receiver::stop_recv_thread()
{
    if (!recv_thread.joinable())
    {
        return;
    }
    recv_stop = true;
    {
        std::lock_guard<std::mutex> lock(mu);
        if (recv_fd >= 0)
        {
            ::shutdown(recv_fd, SHUT_RDWR);
            ::close(recv_fd);
            recv_fd = -1;
        }
    }
    q_cv.notify_all();
    recv_thread.join();
    recv_stop = false;
}

void stream_receiver::recv_thread_main()
{
    /* Bounded wait so block expiry and the in-order emit queue advance even
     * when the wire goes quiet (e.g. tail of a burst loss). */
    constexpr int k_poll_ms = 10;
    uint8_t       buf[2048];
    while (!recv_stop)
    {
        int fd = -1;
        {
            std::lock_guard<std::mutex> lock(mu);
            fd = recv_fd;
        }
        if (fd < 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        pollfd pfd {};
        pfd.fd = fd;
        pfd.events = POLLIN;
        const int pr = poll(&pfd, 1, k_poll_ms);
        if (recv_stop)
        {
            break;
        }
        if (pr < 0)
        {
            if (EINTR == errno)
            {
                continue;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        if (pr > 0)
        {
            /* Drain everything that is ready before ticking. */
            for (;;)
            {
                const ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
                if (n <= 0)
                {
                    break;
                }
                ingest_datagram(buf, static_cast<size_t>(n));
            }
        }

        std::vector<std::vector<uint8_t>> payloads;
        {
            std::lock_guard<std::mutex> lock(mu);
            fec.poll_rx(&payloads);
            note_fec_output_gaps(fec, fec_gap_count);
            fec_lost = fec.decode_fail();
        }
        enqueue_payloads(&payloads);
    }
}

stream_receiver_counters stream_receiver::link_counters_snapshot() const
{
    std::lock_guard<std::mutex> lock(mu);
    stream_receiver_counters c;
    c.udp_packet_received = udp_packet_received;
    c.fec_packet_received = fec_packet_received;
    c.udp_gap_count = udp_gap_count;
    c.fec_gap_count = fec_gap_count;
    c.fec_air_shard_received = fec_air_shard_received;
    return c;
}

int stream_receiver::open()
{
    std::lock_guard<std::mutex> lock(mu);
    if (opened)
    {
        return 0;
    }

    if (listen_spec.empty())
    {
        return -EINVAL;
    }

    char host[128];
    int  port = 0;
    if (parse_host_port(listen_spec, host, sizeof(host), &port) < 0)
    {
        if (listen_spec.size() > 0 && listen_spec[0] == ':')
        {
            std::string p = "0";
            p += listen_spec;
            if (parse_host_port(p, host, sizeof(host), &port) < 0)
            {
                return -EINVAL;
            }
        }
        else
        {
            return -EINVAL;
        }
    }

    recv_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (recv_fd < 0)
    {
        return -errno;
    }

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (0 == std::strcmp(host, "0") || 0 == std::strcmp(host, "0.0.0.0") ||
        0 == std::strcmp(host, ""))
    {
        addr.sin_addr.s_addr = INADDR_ANY;
    }
    else if (inet_pton(AF_INET, host, &addr.sin_addr) != 1)
    {
        ::close(recv_fd);
        recv_fd = -1;
        return -EINVAL;
    }

    if (bind(recv_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
    {
        const int err = errno;
        std::fprintf(stderr, "stream_receiver: bind %s:%d failed: %s\n", host, port,
                     std::strerror(err));
        ::close(recv_fd);
        recv_fd = -1;
        return -err;
    }

    constexpr int k_sock_buf = 16 * 1024 * 1024;
    (void)setsockopt(recv_fd, SOL_SOCKET, SO_RCVBUF, &k_sock_buf, sizeof(k_sock_buf));

    udp_packet_received = 0;
    fec_packet_received = 0;
    udp_gap_count = 0;
    fec_gap_count = 0;
    fec_air_shard_received = 0;
    last_udp_seq = 0;
    have_udp_seq = false;
    fec_payload_sequence = 0;
    opened = true;
    recv_stop = false;
    recv_thread = std::thread(&stream_receiver::recv_thread_main, this);

    std::fprintf(stderr, "stream_receiver: listen %s:%d\n", host, port);
    return 0;
}

void stream_receiver::close()
{
    stop_recv_thread();

    std::lock_guard<std::mutex> lock(mu);
    opened = false;

    {
        std::lock_guard<std::mutex> qlock(q_mu);
        while (!payload_queue.empty())
        {
            payload_queue.pop_front();
        }
    }
    q_cv.notify_all();
}

int stream_receiver::output(uint8_t port, data_packet &out, int timeout_ms)
{
    if (0 != port)
    {
        return -EINVAL;
    }

    std::unique_lock<std::mutex> lock(q_mu);
    if (payload_queue.empty() && timeout_ms != 0)
    {
        if (timeout_ms < 0)
        {
            q_cv.wait(lock, [this] { return !payload_queue.empty(); });
        }
        else
        {
            q_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                          [this] { return !payload_queue.empty(); });
        }
    }

    if (payload_queue.empty())
    {
        return -EAGAIN;
    }

    const size_t egress_bytes =
        data_packet::cast<sock_data>(payload_queue.front()).buf.size;
    out = std::move(payload_queue.front());
    payload_queue.pop_front();
    lock.unlock();
    {
        std::lock_guard<std::mutex> slock(mu);
        update_kbps_window(egress_rate_t0, egress_rate_bytes, egress_kbps, egress_bytes);
    }
    return 0;
}

int stream_receiver::configure(uint64_t /*key*/, int64_t /*value*/)
{
    return -ENOTSUP;
}

int stream_receiver::query(uint64_t /*key*/, int64_t * /*value*/) const
{
    return -ENOTSUP;
}

int stream_receiver::configure(std::string_view key, std::string_view *value)
{
    if (nullptr == value)
    {
        return -EINVAL;
    }

    if ("listen" == key)
    {
        std::lock_guard<std::mutex> lock(mu);
        listen_spec.assign(value->data(), value->size());
        return 0;
    }
    return -ENOTSUP;
}

int stream_receiver::query(std::string_view key, std::string_view *value) const
{
    if (nullptr == value)
    {
        return -EINVAL;
    }

    if ("listen" == key)
    {
        std::lock_guard<std::mutex> lock(mu);
        query_buf = listen_spec;
        *value = query_buf;
        return 0;
    }
    if ("out_rate" == key)
    {
        std::lock_guard<std::mutex> lock(mu);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f", static_cast<double>(egress_kbps));
        query_buf = buf;
        *value = query_buf;
        return 0;
    }
    if ("udp_packet_received" == key || "fec_packet_received" == key || "udp_gap_count" == key ||
        "fec_gap_count" == key || "fec_air_shard_received" == key)
    {
        std::lock_guard<std::mutex> lock(mu);
        uint64_t n = 0;
        if ("udp_packet_received" == key)
        {
            n = udp_packet_received;
        }
        else if ("fec_packet_received" == key)
        {
            n = fec_packet_received;
        }
        else if ("udp_gap_count" == key)
        {
            n = udp_gap_count;
        }
        else if ("fec_gap_count" == key)
        {
            n = fec_gap_count;
        }
        else
        {
            n = fec_air_shard_received;
        }
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%" PRIu64, n);
        query_buf = buf;
        *value = query_buf;
        return 0;
    }
    if ("fec_recovered" == key || "fec_failures" == key)
    {
        std::lock_guard<std::mutex> lock(mu);
        char buf[32];
        const uint64_t n = ("fec_recovered" == key) ? fec_rec : fec_lost;
        std::snprintf(buf, sizeof(buf), "%" PRIu64, n);
        query_buf = buf;
        *value = query_buf;
        return 0;
    }
    if ("stats" == key)
    {
        std::lock_guard<std::mutex> lock(mu);
        char buf[320];
        std::snprintf(buf, sizeof(buf),
                      "udp_packet_received=%" PRIu64 " fec_packet_received=%" PRIu64
                      " udp_gap_count=%" PRIu64 " fec_gap_count=%" PRIu64
                      " fec_air_shard_received=%" PRIu64
                      " bytes=%" PRIu64 " wire_bytes=%" PRIu64 " dropped=%" PRIu64
                      " out_rate=%.1f fec_recovered=%" PRIu64 " fec_failures=%" PRIu64,
                      udp_packet_received, fec_packet_received, udp_gap_count, fec_gap_count,
                      fec_air_shard_received, recv_bytes,
                      recv_wire_bytes, recv_dropped, static_cast<double>(egress_kbps), fec_rec,
                      fec_lost);
        query_buf = buf;
        *value = query_buf;
        return 0;
    }
    return -ENOTSUP;
}

}  // namespace vstreamer
