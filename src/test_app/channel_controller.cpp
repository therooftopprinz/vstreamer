#include "test_app/channel_controller.hpp"

#include "components/encoder_cbr_logic.hpp"
#include "core/metrics.hpp"

#include <cerrno>
#include <cinttypes>
#include <cstdlib>
#include <cstdio>
#include <cstring>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <string>
#include <thread>

namespace vstreamer::test_app
{
namespace
{

double now_sec()
{
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

uint32_t rng32(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x != 0 ? x : 1;
    return *state;
}

void trim_inplace(char *s)
{
    if (nullptr == s)
    {
        return;
    }
    char *start = s;
    while (*start == ' ' || *start == '\t' || *start == '\r')
    {
        start++;
    }
    if (start != s)
    {
        std::memmove(s, start, std::strlen(start) + 1);
    }
    size_t len = std::strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r'))
    {
        s[len - 1] = '\0';
        len--;
    }
}

}  // namespace

channel_controller::channel_controller() = default;

channel_controller::~channel_controller()
{
    stop();
}

void channel_controller::set_max_kbps(double kbps)
{
    std::lock_guard<std::mutex> lock(cfg_mu);
    max_kbps_limit = kbps < 0. ? 0. : kbps;
    const double t = now_sec();
    {
        std::lock_guard<std::mutex> rlock(fwd.rate_mu);
        fwd.rate_window_start = t;
        fwd.rate_window_bytes = 0;
    }
    {
        std::lock_guard<std::mutex> rlock(rev.rate_mu);
        rev.rate_window_start = t;
        rev.rate_window_bytes = 0;
    }
}

void channel_controller::set_constant_loss(double pct)
{
    std::lock_guard<std::mutex> lock(cfg_mu);
    if (pct < 0.)
    {
        loss_pct = 0.;
    }
    else if (pct > 100.)
    {
        loss_pct = 100.;
    }
    else
    {
        loss_pct = pct;
    }
}

double channel_controller::max_kbps() const
{
    std::lock_guard<std::mutex> lock(cfg_mu);
    return max_kbps_limit;
}

double channel_controller::constant_loss() const
{
    std::lock_guard<std::mutex> lock(cfg_mu);
    return loss_pct;
}

channel_controller::forward_stats channel_controller::forward_stats_snapshot() const
{
    forward_stats s;
    s.pkts_in = fwd.pkts_in;
    s.pkts_out = fwd.pkts_out;
    s.bytes_in = fwd.bytes_in;
    s.bytes_out = fwd.bytes_out;
    s.dropped_rate = fwd.dropped_rate;
    s.dropped_loss = fwd.dropped_loss;
    return s;
}

bool channel_controller::should_drop_rate(direction_state &dir, size_t pkt_bytes)
{
    double limit = 0.;
    {
        std::lock_guard<std::mutex> lock(cfg_mu);
        limit = max_kbps_limit;
    }
    if (limit <= 0.)
    {
        return false;
    }

    const double t = now_sec();
    std::lock_guard<std::mutex> lock(dir.rate_mu);
    if (dir.rate_window_start <= 0. || (t - dir.rate_window_start) >= 1.0)
    {
        dir.rate_window_start = t;
        dir.rate_window_bytes = 0;
    }

    const double max_bytes = limit * 1000.0 / 8.0;
    if (static_cast<double>(dir.rate_window_bytes + pkt_bytes) > max_bytes)
    {
        return true;
    }
    dir.rate_window_bytes += static_cast<uint64_t>(pkt_bytes);
    return false;
}

bool channel_controller::should_drop_loss(direction_state &dir)
{
    double pct = 0.;
    {
        std::lock_guard<std::mutex> lock(cfg_mu);
        pct = loss_pct;
    }
    if (pct <= 0.)
    {
        return false;
    }
    if (pct >= 100.)
    {
        return true;
    }
    const uint32_t r = rng32(&dir.rng);
    const double   u = static_cast<double>(r % 10000) / 10000.0;
    return u < (pct / 100.0);
}

void channel_controller::forward_packet(direction_state &dir, const uint8_t *buf, size_t n)
{
    dir.pkts_in++;
    dir.bytes_in += static_cast<uint64_t>(n);

    if (should_drop_rate(dir, n))
    {
        dir.dropped_rate++;
        return;
    }
    if (should_drop_loss(dir))
    {
        dir.dropped_loss++;
        return;
    }

    if (!dir.have_egress || dir.egress_fd < 0)
    {
        return;
    }

    const ssize_t sent =
        sendto(dir.egress_fd, buf, n, 0, reinterpret_cast<sockaddr *>(&dir.egress_addr),
               sizeof(dir.egress_addr));
    if (sent == static_cast<ssize_t>(n))
    {
        dir.pkts_out++;
        dir.bytes_out += static_cast<uint64_t>(n);
    }
}

void channel_controller::relay_thread_main()
{
    uint8_t buf[2048];

    while (!relay_stop.load())
    {
        pollfd fds[2];
        int    nfds = 0;

        if (fwd.ingress_fd >= 0)
        {
            fds[nfds].fd = fwd.ingress_fd;
            fds[nfds].events = POLLIN;
            nfds++;
        }
        if (rev_enabled && rev.ingress_fd >= 0)
        {
            fds[nfds].fd = rev.ingress_fd;
            fds[nfds].events = POLLIN;
            nfds++;
        }

        if (nfds == 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        const int pr = poll(fds, nfds, 50);
        if (pr < 0)
        {
            if (relay_stop.load())
            {
                break;
            }
            continue;
        }
        if (pr == 0)
        {
            continue;
        }

        auto drain_ingress = [&](direction_state &dir) {
            while (!relay_stop.load())
            {
                const ssize_t n =
                    recv(dir.ingress_fd, buf, sizeof(buf), MSG_DONTWAIT);
                if (n > 0)
                {
                    forward_packet(dir, buf, static_cast<size_t>(n));
                    continue;
                }
                if (n < 0 && (EAGAIN == errno || EWOULDBLOCK == errno))
                {
                    break;
                }
                break;
            }
        };

        int idx = 0;
        if (fwd.ingress_fd >= 0)
        {
            if ((fds[idx].revents & POLLIN) != 0)
            {
                drain_ingress(fwd);
            }
            idx++;
        }
        if (rev_enabled && rev.ingress_fd >= 0)
        {
            if ((fds[idx].revents & POLLIN) != 0)
            {
                drain_ingress(rev);
            }
        }
    }
}

void channel_controller::set_pipeline_metrics(const vstreamer::metrics *source)
{
    pipeline_metrics = source;
}

void channel_controller::set_pipeline_metrics_refresh(std::function<void()> refresh)
{
    pipeline_metrics_refresh = std::move(refresh);
}

void channel_controller::set_cbr_pid_logic(vstreamer::encoder_cbr_logic *logic)
{
    cbr_pid_logic = logic;
}

void channel_controller::send_pipeline_metrics(int reply_fd, const sockaddr_in &reply)
{
    if (nullptr == pipeline_metrics)
    {
        const char *msg = "err metrics not configured\n";
        sendto(reply_fd, msg, std::strlen(msg), 0, reinterpret_cast<const sockaddr *>(&reply),
               sizeof(reply));
        return;
    }

    /* Snapshot only: refresh runs on stream_sdl metrics thread (not here). A full
     * refresh on every UDP get blocked the console past short nc timeouts and looked
     * like a blank blink with clear + poll loops. */
    const std::string report = pipeline_metrics->to_string();
    if (report.empty())
    {
        const char *msg = "err metrics empty\n";
        sendto(reply_fd, msg, std::strlen(msg), 0, reinterpret_cast<const sockaddr *>(&reply),
               sizeof(reply));
        return;
    }

    sendto(reply_fd, report.data(), report.size(), 0, reinterpret_cast<const sockaddr *>(&reply),
           sizeof(reply));
}

void channel_controller::handle_console_line(const char *line, int reply_fd,
                                             const sockaddr_in &reply)
{
    char work[256];
    std::snprintf(work, sizeof(work), "%s", line);
    trim_inplace(work);

    if (0 == std::strcmp(work, "help") || 0 == std::strcmp(work, "h") ||
        0 == std::strcmp(work, "?"))
    {
        static const char help_msg[] =
            "set_max_kbps <kbps>\n"
            "set_constant_loss <pct>\n"
            "set_cbr_pid kp=.. step_down=.. step_up=.. (see cbr_pid)\n"
            "set_cbr_pid defaults | reset\n"
            "cbr_pid | get_cbr_pid\n"
            "ping\n"
            "stats\n"
            "metrics\n"
            "get\n"
            "(empty line)  pipeline metrics\n";
        sendto(reply_fd, help_msg, std::strlen(help_msg), 0,
               reinterpret_cast<const sockaddr *>(&reply), sizeof(reply));
        return;
    }

    if (0 == std::strcmp(work, "ping"))
    {
        const char *msg = "pong\n";
        sendto(reply_fd, msg, std::strlen(msg), 0, reinterpret_cast<const sockaddr *>(&reply),
               sizeof(reply));
        return;
    }

    if (0 == std::strncmp(work, "set_max_kbps ", 13))
    {
        const char *arg = work + 13;
        char       *end = nullptr;
        const double v = strtod(arg, &end);
        if (end == arg)
        {
            const char *msg = "err bad value\n";
            sendto(reply_fd, msg, std::strlen(msg), 0, reinterpret_cast<const sockaddr *>(&reply),
                   sizeof(reply));
            return;
        }
        set_max_kbps(v);
        const char *msg = "ok\n";
        sendto(reply_fd, msg, std::strlen(msg), 0, reinterpret_cast<const sockaddr *>(&reply),
               sizeof(reply));
        return;
    }

    if (0 == std::strncmp(work, "set_constant_loss ", 18))
    {
        const char *arg = work + 18;
        char       *end = nullptr;
        const double v = strtod(arg, &end);
        if (end == arg)
        {
            const char *msg = "err bad value\n";
            sendto(reply_fd, msg, std::strlen(msg), 0, reinterpret_cast<const sockaddr *>(&reply),
                   sizeof(reply));
            return;
        }
        set_constant_loss(v);
        const char *msg = "ok\n";
        sendto(reply_fd, msg, std::strlen(msg), 0, reinterpret_cast<const sockaddr *>(&reply),
               sizeof(reply));
        return;
    }

    if (nullptr != cbr_pid_logic &&
        (0 == std::strncmp(work, "set_cbr_pid ", 12) || 0 == std::strcmp(work, "cbr_pid") ||
         0 == std::strcmp(work, "get_cbr_pid")))
    {
        char reply[512];
        cbr_pid_logic->handle_pid_console_line(work, reply, sizeof(reply));
        sendto(reply_fd, reply, std::strlen(reply), 0, reinterpret_cast<const sockaddr *>(&reply),
               sizeof(reply));
        return;
    }

    if (0 == std::strcmp(work, "stats"))
    {
        char msg[320];
        std::snprintf(msg, sizeof(msg),
                      "fwd in=%" PRIu64 " out=%" PRIu64 " drop_rate=%" PRIu64
                      " drop_loss=%" PRIu64
                      " | rev in=%" PRIu64 " out=%" PRIu64 " drop_rate=%" PRIu64
                      " drop_loss=%" PRIu64 " | max_kbps=%.0f loss_pct=%.2f\n",
                      fwd.pkts_in, fwd.pkts_out, fwd.dropped_rate, fwd.dropped_loss, rev.pkts_in,
                      rev.pkts_out, rev.dropped_rate, rev.dropped_loss, max_kbps(),
                      constant_loss());
        sendto(reply_fd, msg, std::strlen(msg), 0, reinterpret_cast<const sockaddr *>(&reply),
               sizeof(reply));
        return;
    }

    if (0 == std::strcmp(work, "metrics") || 0 == std::strcmp(work, "get"))
    {
        send_pipeline_metrics(reply_fd, reply);
        return;
    }

    if ('\0' == work[0])
    {
        send_pipeline_metrics(reply_fd, reply);
        return;
    }

    const char *msg = "err unknown\n";
    sendto(reply_fd, msg, std::strlen(msg), 0, reinterpret_cast<const sockaddr *>(&reply),
           sizeof(reply));
}

void channel_controller::console_thread_main()
{
    uint8_t buf[512];
    while (!console_stop.load())
    {
        if (console_fd < 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        sockaddr_in from {};
        socklen_t   from_len = sizeof(from);
        const ssize_t n =
            recvfrom(console_fd, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr *>(&from),
                     &from_len);
        if (n <= 0)
        {
            if (console_stop.load())
            {
                break;
            }
            continue;
        }
        buf[n] = '\0';

        char *nl = static_cast<char *>(std::memchr(buf, '\n', static_cast<size_t>(n)));
        if (nullptr != nl)
        {
            *nl = '\0';
        }
        handle_console_line(reinterpret_cast<char *>(buf), console_fd, from);
    }
}

int channel_controller::setup_direction(direction_state &dir, int ingress_port,
                                      const char *egress_host, int egress_port)
{
    dir.ingress_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (dir.ingress_fd < 0)
    {
        return -errno;
    }

    sockaddr_in in {};
    in.sin_family = AF_INET;
    in.sin_addr.s_addr = htonl(INADDR_ANY);
    in.sin_port = htons(static_cast<uint16_t>(ingress_port));
    if (bind(dir.ingress_fd, reinterpret_cast<sockaddr *>(&in), sizeof(in)) < 0)
    {
        const int err = -errno;
        ::close(dir.ingress_fd);
        dir.ingress_fd = -1;
        return err;
    }

    constexpr int k_sock_buf = 16 * 1024 * 1024;
    (void)setsockopt(dir.ingress_fd, SOL_SOCKET, SO_RCVBUF, &k_sock_buf, sizeof(k_sock_buf));

    dir.egress_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (dir.egress_fd < 0)
    {
        const int err = -errno;
        ::close(dir.ingress_fd);
        dir.ingress_fd = -1;
        return err;
    }

    std::memset(&dir.egress_addr, 0, sizeof(dir.egress_addr));
    dir.egress_addr.sin_family = AF_INET;
    dir.egress_addr.sin_port = htons(static_cast<uint16_t>(egress_port));
    if (inet_pton(AF_INET, egress_host, &dir.egress_addr.sin_addr) != 1)
    {
        ::close(dir.egress_fd);
        dir.egress_fd = -1;
        ::close(dir.ingress_fd);
        dir.ingress_fd = -1;
        return -EINVAL;
    }
    (void)setsockopt(dir.egress_fd, SOL_SOCKET, SO_SNDBUF, &k_sock_buf, sizeof(k_sock_buf));

    dir.have_egress = true;
    return 0;
}

void channel_controller::teardown_direction(direction_state &dir)
{
    if (dir.ingress_fd >= 0)
    {
        ::shutdown(dir.ingress_fd, SHUT_RDWR);
        ::close(dir.ingress_fd);
        dir.ingress_fd = -1;
    }
    if (dir.egress_fd >= 0)
    {
        ::close(dir.egress_fd);
        dir.egress_fd = -1;
    }
    dir.have_egress = false;
}

int channel_controller::start(int ingress_port, const char *egress_host, int egress_port,
                              int reverse_ingress_port, const char *reverse_egress_host,
                              int reverse_egress_port)
{
    stop_relay();

    const int ret = setup_direction(fwd, ingress_port, egress_host, egress_port);
    if (ret < 0)
    {
        return ret;
    }

    rev_enabled = false;
    if (reverse_ingress_port > 0 && nullptr != reverse_egress_host && reverse_egress_port > 0)
    {
        const int rret =
            setup_direction(rev, reverse_ingress_port, reverse_egress_host, reverse_egress_port);
        if (rret < 0)
        {
            teardown_direction(fwd);
            return rret;
        }
        rev_enabled = true;
        rev.rng = 0xA5A5A5A5u;
    }

    relay_stop = false;
    relay_thread = std::thread(&channel_controller::relay_thread_main, this);

    std::fprintf(stderr, "channel_controller: fwd :%d -> %s:%d\n", ingress_port, egress_host,
                 egress_port);
    if (rev_enabled)
    {
        std::fprintf(stderr, "channel_controller: rev :%d -> %s:%d\n", reverse_ingress_port,
                     reverse_egress_host, reverse_egress_port);
    }
    return 0;
}

int channel_controller::start_console(int console_port)
{
    stop_console();

    console_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (console_fd < 0)
    {
        return -errno;
    }

    sockaddr_in in {};
    in.sin_family = AF_INET;
    in.sin_addr.s_addr = htonl(INADDR_ANY);
    in.sin_port = htons(static_cast<uint16_t>(console_port));
    if (bind(console_fd, reinterpret_cast<sockaddr *>(&in), sizeof(in)) < 0)
    {
        const int err = -errno;
        ::close(console_fd);
        console_fd = -1;
        return err;
    }

    console_stop = false;
    console_thread = std::thread(&channel_controller::console_thread_main, this);
    std::fprintf(stderr, "channel_controller: console udp :%d\n", console_port);
    return 0;
}

void channel_controller::stop_relay()
{
    relay_stop = true;
    teardown_direction(fwd);
    if (rev_enabled)
    {
        teardown_direction(rev);
    }
    rev_enabled = false;

    if (relay_thread.joinable())
    {
        relay_thread.join();
    }
    relay_stop = false;
}

void channel_controller::stop_console()
{
    console_stop = true;
    if (console_fd >= 0)
    {
        ::shutdown(console_fd, SHUT_RDWR);
        ::close(console_fd);
        console_fd = -1;
    }
    if (console_thread.joinable())
    {
        console_thread.join();
    }
    console_stop = false;
}

void channel_controller::stop()
{
    stop_console();
    stop_relay();
}

}  // namespace vstreamer::test_app
