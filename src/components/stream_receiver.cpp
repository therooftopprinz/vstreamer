#include "components/stream_receiver.hpp"

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

namespace
{

uint64_t note_rtp_sequence_gap(const uint8_t *buf, size_t n, uint16_t &last_seq, bool &have_seq)
{
    if (n < 12 || (buf[0] & 0xC0) != 0x80)
    {
        return 0;
    }
    const uint16_t seq = static_cast<uint16_t>((static_cast<uint16_t>(buf[2]) << 8) | buf[3]);
    if (!have_seq)
    {
        have_seq = true;
        last_seq = seq;
        return 0;
    }
    const uint16_t next = static_cast<uint16_t>(last_seq + 1);
    uint64_t       lost = 0;
    if (seq != next)
    {
        lost = static_cast<uint16_t>(seq - next);
    }
    last_seq = seq;
    return lost;
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
    uint8_t buf[2048];
    while (!recv_stop)
    {
        if (recv_fd < 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        ssize_t n = recv(recv_fd, buf, sizeof(buf), 0);
        if (n <= 0)
        {
            if (recv_stop)
            {
                break;
            }
            continue;
        }

        for (;;)
        {
            {
                std::lock_guard<std::mutex> lock(mu);
                recv_lost += note_rtp_sequence_gap(buf, static_cast<size_t>(n), last_rtp_seq,
                                                   have_rtp_seq);
            }

            data_packet pkt;
            uint8_t    *copy = pool.acquire(static_cast<size_t>(n));
            if (nullptr == copy)
            {
                {
                    std::lock_guard<std::mutex> lock(mu);
                    recv_dropped++;
                }
                break;
            }

            std::memcpy(copy, buf, static_cast<size_t>(n));
            auto sd = std::make_unique<sock_data>();
            sd->pts = 0;
            sd->buf.reset(copy, static_cast<size_t>(n), &packet_pool::release);
            pkt.reset(std::move(sd));

            {
                std::lock_guard<std::mutex> lock(mu);
                recv_pkts++;
                recv_bytes += static_cast<uint64_t>(n);
            }

            {
                std::lock_guard<std::mutex> lock(q_mu);
                if (payload_queue.size() >= k_queue_depth)
                {
                    payload_queue.pop_front();
                    recv_dropped++;
                }
                payload_queue.push_back(std::move(pkt));
            }
            q_cv.notify_all();

            n = recv(recv_fd, buf, sizeof(buf), MSG_DONTWAIT);
            if (n <= 0)
            {
                if (n < 0 && (EAGAIN == errno || EWOULDBLOCK == errno))
                {
                    break;
                }
                break;
            }
        }
    }
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

    recv_lost = 0;
    last_rtp_seq = 0;
    have_rtp_seq = false;
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

    out = std::move(payload_queue.front());
    payload_queue.pop_front();
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
    if ("stats" == key)
    {
        std::lock_guard<std::mutex> lock(mu);
        char buf[64];
        std::snprintf(buf, sizeof(buf),
                      "pkts=%" PRIu64 " bytes=%" PRIu64 " dropped=%" PRIu64 " lost=%" PRIu64,
                      recv_pkts, recv_bytes, recv_dropped, recv_lost);
        query_buf = buf;
        *value = query_buf;
        return 0;
    }
    return -ENOTSUP;
}

}  // namespace vstreamer
