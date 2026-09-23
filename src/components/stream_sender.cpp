#include "components/stream_sender.hpp"

#include "core/host_util.hpp"
#include "core/key_util.hpp"

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#include <chrono>
#include <thread>
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

void stream_sender::update_telemetry_locked()
{
    size_t qdepth = 0;
    {
        std::lock_guard<std::mutex> qlock(q_mu);
        qdepth = queue.size();
    }
    tel.flow = static_cast<float>(qdepth) / static_cast<float>(k_queue_depth);
    tel.channel_loss = link_loss;
}

void stream_sender::set_channel_loss(float loss)
{
    std::lock_guard<std::mutex> lock(mu);
    if (loss < 0.f)
    {
        loss = 0.f;
    }
    else if (loss > 1.f)
    {
        loss = 1.f;
    }
    link_loss = loss;
    tel.channel_loss = link_loss;
}

stream_telemetry stream_sender::telemetry_snapshot() const
{
    std::lock_guard<std::mutex> lock(mu);
    return tel;
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
        data_packet pkt;
        {
            std::unique_lock<std::mutex> lock(q_mu);
            q_cv.wait_for(lock, std::chrono::milliseconds(50),
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
        pace_wire_send(sd.buf.size);
        const ssize_t n = sendto(send_fd, sd.buf.data, sd.buf.size, 0,
                                    reinterpret_cast<sockaddr *>(&dst), sizeof(dst));
        if (n < 0)
        {
            continue;
        }
        {
            const double t = now_sec();
            std::lock_guard<std::mutex> lock(mu);
            pkts_sent++;
            bytes_sent += static_cast<uint64_t>(n);
            if (egress_rate_t0 <= 0.)
            {
                egress_rate_t0 = t;
            }
            egress_rate_bytes += static_cast<uint64_t>(n);
            const double dt = t - egress_rate_t0;
            if (dt >= 0.5)
            {
                tel.egress_kbps =
                    static_cast<float>(egress_rate_bytes * 8.0 / dt / 1000.0);
                egress_rate_t0 = t;
                egress_rate_bytes = 0;
            }
            update_telemetry_locked();
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
    opened = true;

    send_stop = false;
    send_thread = std::thread(&stream_sender::send_thread_main, this);

    std::fprintf(stderr, "stream_sender: udp://%s:%d mtu=%d\n", host, port, mtu);
    return 0;
}

void stream_sender::close()
{
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
    data_packet      copy;
    uint8_t         *buf = pool.acquire(src.buf.size);
    if (nullptr == buf)
    {
        std::lock_guard<std::mutex> lock(mu);
        dropped++;
        update_telemetry_locked();
        return 0;
    }
    std::memcpy(buf, src.buf.data, src.buf.size);
    auto sd = std::make_unique<sock_data>();
    sd->pts = src.pts;
    sd->buf.reset(buf, src.buf.size, &packet_pool::release);
    copy.reset(std::move(sd));

    {
        std::lock_guard<std::mutex> lock(q_mu);
        if (queue.size() >= k_queue_depth)
        {
            queue.pop_front();
            std::lock_guard<std::mutex> slock(mu);
            dropped++;
        }
        queue.push_back(std::move(copy));
    }
    {
        std::lock_guard<std::mutex> lock(mu);
        update_telemetry_locked();
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
    if ("stats" == key)
    {
        std::lock_guard<std::mutex> lock(mu);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "pkts=%" PRIu64 " bytes=%" PRIu64 " dropped=%" PRIu64,
                      pkts_sent, bytes_sent, dropped);
        query_buf = buf;
        *value = query_buf;
        return 0;
    }
    if ("telemetry" == key)
    {
        std::lock_guard<std::mutex> lock(mu);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "flow=%.3f loss=%.4f rssi=%.1f",
                      static_cast<double>(tel.flow), static_cast<double>(tel.channel_loss),
                      static_cast<double>(tel.rssi));
        query_buf = buf;
        *value = query_buf;
        return 0;
    }
    return -ENOTSUP;
}

}  // namespace vstreamer
