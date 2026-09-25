/*
 * Bench: synthetic noise through RTP loopback + impaired UDP channel → SDL.
 *
 * Pipeline threads (default CPU affinity 0–3 on Linux):
 *   CPU0: noise_source or v4l2_source (--source) → queue
 *   CPU1: jpeg_decoder_multicore → queue
 *   CPU2: h264_encoder_mpp → rtp_h264_pay → stream_sender
 *   CPU3: stream_receiver → rtp_h264_depay → h264_decoder_mpp → display sink
 * Rate: encoder CBR/QP at open; adjust via UDP set_encode_cbr / set_encode_qp.
 * Link: stream_sender → channel_controller (fwd) → stream_receiver
 *       return traffic → channel_controller (rev) → configured egress port
 *
 * Default UDP ports (channel_ports.hpp): fwd 5000→5001, rev 5002→5003, console 5090.
 *
 * channel_controller console (UDP, newline-terminated):
 *   help | h | ?
 *   set_max_kbps <kbps>
 *   set_constant_loss <pct>
 *   set_fec none | set_fec_k <k> | set_fec_n <n>  (stream_sender RS_BLOCK_ERASURE)
 *   set_encode_cbr <kbps> | set_encode_qp <qp> | set_gop <gop> | get_metric <name>
 *   ping / stats / metrics | get | empty line → pipeline metrics report
 *
 * Low-latency queue defaults (override with env):
 *   VSTREAMER_PIPE_QUEUE_DEPTH (default 2)
 *   VSTREAMER_PRESENT_QUEUE_DEPTH (default 1)
 *   VSTREAMER_RX_AU_QUEUE_DEPTH (default 4)
 * Metric latency.* / h264_encoder.latency_ms / h264_decoder.latency_ms /
 * sdl_sink.latency_ms / stream_sdl.glass_latency_ms (capture-to-stage ms).
 *
 * Per-stage latency lines (stderr): --diag or VSTREAMER_LOG_STAGE_LATENCY=1
 * Optional: VSTREAMER_STAGE_LATENCY_EVERY=N (log every Nth frame by pts, default 1).
 */

#include "components/components.hpp"
#include "test_app/channel_controller.hpp"
#include "test_app/channel_ports.hpp"

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <algorithm>
#include <atomic>
#include <deque>
#include <chrono>
#include <csignal>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <cstdlib>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include "core/component_source.hpp"
#include "core/metrics.hpp"
#include "core/stream_telemetry.hpp"
#include "core/thread_affinity.hpp"
#include "core/time_util.hpp"

namespace vstreamer
{
namespace
{

#if !defined(ENABLE_NOISE_SOURCE) || !defined(ENABLE_JPEG_DECODER_MULTICORE) ||     \
    !defined(ENABLE_RTP_H264_PAY) || !defined(ENABLE_STREAM_SENDER) ||              \
    !defined(ENABLE_STREAM_RECEIVER) || !defined(ENABLE_RTP_H264_DEPAY) ||        \
    !defined(ENABLE_H264_DECODER_MPP) || !defined(ENABLE_SDL_SINK)
#error "stream_sdl requires noise, jpeg_decoder, stream_*, rtp h264, mpp decode, sdl"
#endif

#if defined(ENABLE_H264_ENCODER_MPP)
using h264_encoder_t = h264_encoder_mpp;
#elif defined(ENABLE_H264_ENCODER_CEDAR)
using h264_encoder_t = h264_encoder_cedar;
#elif defined(ENABLE_H264_ENCODER_INTEL)
using h264_encoder_t = h264_encoder_intel;
#else
#error "stream_sdl requires ENABLE_H264_ENCODER_MPP, CEDAR, or INTEL"
#endif

std::atomic<bool> g_run {true};
std::atomic<bool> g_diag {false};
std::atomic<bool> g_bench_metrics_log {false};
std::atomic<bool> g_skip_decode {false};
std::atomic<bool> g_dec_opened {false};

int ensure_decoder_open(h264_decoder_mpp *dec)
{
    if (g_skip_decode.load() || nullptr == dec)
    {
        return -EINVAL;
    }
    if (g_dec_opened.load())
    {
        return 0;
    }
    const int r = dec->open();
    if (r < 0)
    {
        return r;
    }
    g_dec_opened = true;
    return 0;
}

/* Serialize Rockchip MPP decode on RX (encode runs on another CPU). */
std::mutex g_mpp_hw_mu;

constexpr int k_cpu_noise = 0;
constexpr int k_cpu_jpeg = 1;
constexpr int k_cpu_encode = 2;
constexpr int k_cpu_rx = 3;

constexpr size_t k_default_pipe_queue_depth = 2;
constexpr size_t k_default_present_queue_depth = 1;
constexpr size_t k_default_rx_au_queue_depth = 4;

std::atomic<int64_t> g_latest_source_pts {0};
std::atomic<int>     g_stream_fps {30};
/* Applied on encode thread (console must not call MPP configure directly). */
std::atomic<int> g_pending_console_cbr_kbps {-1};
std::atomic<int> g_pending_console_qp {-1};
std::atomic<int> g_pending_console_gop {-1};
std::atomic<double>  g_glass_latency_ms {0.0};
std::atomic<double>  g_latency_source_ms {0.0};
std::atomic<double>  g_latency_jpeg_ms {0.0};
std::atomic<double>  g_latency_enc_in_ms {0.0};
std::atomic<double>  g_latency_enc_out_ms {0.0};
std::atomic<double>  g_latency_depay_ms {0.0};
std::atomic<double>  g_latency_dec_in_ms {0.0};
std::atomic<double>  g_latency_dec_out_ms {0.0};
std::atomic<double>  g_latency_present_ms {0.0};

[[nodiscard]] size_t queue_depth_from_env(const char *name, size_t default_val, size_t max_val)
{
    const char *v = std::getenv(name);
    if (nullptr == v || v[0] == '\0')
    {
        return default_val;
    }
    char *end = nullptr;
    const unsigned long n = std::strtoul(v, &end, 10);
    if (end == v || n == 0)
    {
        return default_val;
    }
    return static_cast<size_t>(std::min(n, static_cast<unsigned long>(max_val)));
}

void note_source_pts(const data_packet &pkt)
{
    if (pkt.get_type() != packet_kind_e::FRAME)
    {
        return;
    }
    const frame_data &f = data_packet::cast<frame_data>(pkt);
    g_latest_source_pts.store(f.pts, std::memory_order_relaxed);
}

[[nodiscard]] size_t packet_frame_bytes(const data_packet &pkt)
{
    if (pkt.get_type() != packet_kind_e::FRAME)
    {
        return 0;
    }
    return data_packet::cast<frame_data>(pkt).buf.size;
}

[[nodiscard]] bool stage_latency_log_enabled()
{
    static const bool env_on = [] {
        const char *v = std::getenv("VSTREAMER_LOG_STAGE_LATENCY");
        return nullptr != v && v[0] != '\0' && 0 != std::strcmp(v, "0");
    }();
    return g_diag.load() || env_on;
}

[[nodiscard]] int stage_latency_log_stride()
{
    static const int every = [] {
        const char *v = std::getenv("VSTREAMER_STAGE_LATENCY_EVERY");
        if (nullptr == v || v[0] == '\0')
        {
            return 1;
        }
        char       *end = nullptr;
        const long n = std::strtol(v, &end, 10);
        return (end != v && n > 0) ? static_cast<int>(n) : 1;
    }();
    return every;
}

void record_stage_latency_ms(const char *stage, const data_packet &pkt, double ms)
{
    if (nullptr == stage)
    {
        return;
    }
    if (0 == std::strcmp(stage, "source"))
    {
        g_latency_source_ms.store(ms, std::memory_order_relaxed);
    }
    else if (0 == std::strcmp(stage, "jpeg_nv12"))
    {
        g_latency_jpeg_ms.store(ms, std::memory_order_relaxed);
    }
    else if (0 == std::strcmp(stage, "enc_in"))
    {
        g_latency_enc_in_ms.store(ms, std::memory_order_relaxed);
    }
    else if (0 == std::strcmp(stage, "enc_out"))
    {
        g_latency_enc_out_ms.store(ms, std::memory_order_relaxed);
    }
    else if (0 == std::strcmp(stage, "depay"))
    {
        g_latency_depay_ms.store(ms, std::memory_order_relaxed);
    }
    else if (0 == std::strcmp(stage, "dec_in"))
    {
        g_latency_dec_in_ms.store(ms, std::memory_order_relaxed);
    }
    else if (0 == std::strcmp(stage, "dec_out"))
    {
        g_latency_dec_out_ms.store(ms, std::memory_order_relaxed);
    }
    else if (0 == std::strcmp(stage, "present"))
    {
        g_latency_present_ms.store(ms, std::memory_order_relaxed);
        g_glass_latency_ms.store(ms, std::memory_order_relaxed);
    }
}

void log_stage_latency(const char *stage, const data_packet &pkt)
{
    if (nullptr == stage || pkt.get_type() != packet_kind_e::FRAME)
    {
        return;
    }
    const frame_data &f = data_packet::cast<frame_data>(pkt);
    if (f.capture_mono_ns <= 0)
    {
        return;
    }
    const int64_t now_ns = steady_mono_ns();
    const double  ms = static_cast<double>(now_ns - f.capture_mono_ns) / 1e6;
    if (ms >= 0.0)
    {
        record_stage_latency_ms(stage, pkt, ms);
    }

    if (!stage_latency_log_enabled())
    {
        return;
    }
    const int stride = stage_latency_log_stride();
    if (stride > 1 && (f.pts % stride) != 0)
    {
        return;
    }
    std::fprintf(stderr, "stage_latency: %-10s %7.2f ms pts=%" PRId64 "\n", stage, ms, f.pts);
}

class pipeline_queue
{
public:
    explicit pipeline_queue(size_t cap) : capacity(cap) {}

    bool push(data_packet pkt, std::atomic<uint64_t> *drops = nullptr)
    {
        std::unique_lock<std::mutex> lock(mu);
        if (!g_run.load())
        {
            return false;
        }
        if (q.size() >= capacity)
        {
            q.pop_front();
            if (nullptr != drops)
            {
                drops->fetch_add(1);
            }
        }
        q.push_back(std::move(pkt));
        cv_pop.notify_one();
        return true;
    }

    [[nodiscard]] size_t size()
    {
        std::lock_guard<std::mutex> lock(mu);
        return q.size();
    }

    bool pop(data_packet &out, int timeout_ms)
    {
        std::unique_lock<std::mutex> lock(mu);
        const auto ready = [this] { return !g_run.load() || !q.empty(); };
        if (timeout_ms < 0)
        {
            cv_pop.wait(lock, ready);
        }
        else if (timeout_ms > 0)
        {
            cv_pop.wait_for(lock, std::chrono::milliseconds(timeout_ms), ready);
        }
        else if (!ready())
        {
            return false;
        }
        if (q.empty())
        {
            return false;
        }
        out = std::move(q.front());
        q.pop_front();
        cv_push.notify_one();
        return true;
    }

private:
    size_t                    capacity;
    std::mutex                mu;
    std::condition_variable   cv_push;
    std::condition_variable   cv_pop;
    std::deque<data_packet>   q;
};

/* Decoded NV12 waiting for SDL (RX enqueues; present thread draws). */
class present_frame_queue
{
public:
    explicit present_frame_queue(size_t cap) : capacity(cap) {}

    bool push(data_packet pkt, std::atomic<uint64_t> *drops = nullptr)
    {
        std::lock_guard<std::mutex> lock(mu);
        if (!g_run.load())
        {
            return false;
        }
        if (q.size() >= capacity)
        {
            q.pop_front();
            if (nullptr != drops)
            {
                (*drops)++;
            }
        }
        q.push_back(std::move(pkt));
        cv_pop.notify_one();
        return true;
    }

    bool pop(data_packet &out, int timeout_ms)
    {
        std::unique_lock<std::mutex> lock(mu);
        const auto ready = [this] { return !g_run.load() || !q.empty(); };
        if (timeout_ms < 0)
        {
            cv_pop.wait(lock, ready);
        }
        else if (timeout_ms > 0)
        {
            cv_pop.wait_for(lock, std::chrono::milliseconds(timeout_ms), ready);
        }
        else if (!ready())
        {
            return false;
        }
        if (q.empty())
        {
            return false;
        }
        out = std::move(q.front());
        q.pop_front();
        return true;
    }

    void wake()
    {
        cv_pop.notify_all();
    }

private:
    size_t                  capacity;
    std::mutex              mu;
    std::condition_variable cv_pop;
    std::deque<data_packet> q;
};

struct bench_diag
{
    std::atomic<uint64_t> tx_noise {0};
    std::atomic<uint64_t> tx_source_bytes {0};
    std::atomic<uint64_t> tx_mjpeg_q_drop {0};
    std::atomic<uint64_t> tx_nv12_q_drop {0};
    std::atomic<uint64_t> tx_jpeg_nv12 {0};
    std::atomic<uint64_t> tx_jpeg_nv12_bytes {0};
    std::atomic<uint64_t> tx_enc_nv12_popped {0};
    std::atomic<uint64_t> tx_nv12 {0};
    std::atomic<uint64_t> tx_enc_input_miss {0};
    std::atomic<uint64_t> tx_enc_in_err {0};
    std::atomic<uint64_t> tx_enc_skip {0};
    std::atomic<uint64_t> tx_enc_out_bytes {0};
    std::atomic<uint64_t> tx_rtp_sock {0};
    std::atomic<uint64_t> rx_udp {0};
    std::atomic<uint64_t> rx_depay_err {0};
    std::atomic<uint64_t> rx_depay_au {0};
    std::atomic<uint64_t> rx_dec_in_ok {0};
    std::atomic<uint64_t> rx_dec_in_bytes {0};
    std::atomic<uint64_t> rx_dec_in_eagain {0};
    std::atomic<uint64_t> rx_dec_in_err {0};
    std::atomic<uint64_t> rx_dec_out_eagain {0};
    std::atomic<uint64_t> rx_dec_out_err {0};
    std::atomic<uint64_t> rx_nv12_out {0};
    std::atomic<uint64_t> rx_nv12_out_bytes {0};
    std::atomic<uint64_t> rx_present_ok {0};
    std::atomic<uint64_t> rx_present_err {0};
    std::atomic<uint64_t> rx_present_q_drop {0};
    std::atomic<uint64_t> rx_au_q_drop {0};
};

struct rx_au_queue
{
    explicit rx_au_queue(size_t cap) : capacity(cap) {}

    size_t capacity = k_default_rx_au_queue_depth;

    void push(data_packet pkt, bench_diag &diag)
    {
        std::lock_guard<std::mutex> lock(mu);
        if (!g_run.load())
        {
            return;
        }
        if (q.size() >= capacity)
        {
            q.pop_front();
            diag.rx_au_q_drop++;
        }
        q.push_back(std::move(pkt));
        cv.notify_one();
    }

    bool pop(data_packet &out, int timeout_ms)
    {
        std::unique_lock<std::mutex> lock(mu);
        const auto                   ready = [this] { return !g_run.load() || !q.empty(); };
        if (timeout_ms < 0)
        {
            cv.wait(lock, ready);
        }
        else if (timeout_ms > 0)
        {
            cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), ready);
        }
        else if (!ready())
        {
            return false;
        }
        if (q.empty())
        {
            return false;
        }
        out = std::move(q.front());
        q.pop_front();
        return true;
    }

    void wake()
    {
        cv.notify_all();
    }

    std::mutex              mu;
    std::condition_variable cv;
    std::deque<data_packet> q;
};

bench_diag g_bench_diag;
metrics    g_pipeline_metrics;
metric     g_metric_seed;
component_source *g_metrics_source = nullptr;
pipeline_queue   *g_metrics_nv12_q = nullptr;

bool query_source_metric_string(component_source *src, const char *key, std::string &out)
{
    if (nullptr == src || nullptr == key)
    {
        return false;
    }
    std::string_view v;
    if (src->query(std::string_view(key), &v) != 0 || v.empty())
    {
        return false;
    }
    out.assign(v.begin(), v.end());
    return true;
}

void store_source_pipeline_metrics(double source_out_fps, double source_out_kbps,
                                   uint64_t source_out_bytes, const char *ts,
                                   jpeg_decoder_multicore *jdec)
{
    std::string device = "noise";
    std::string media_type = "mjpeg";
    std::string pixel_type = "mjpeg";
    int         width = 0;
    int         height = 0;

    if (nullptr != g_metrics_source)
    {
        (void)query_source_metric_string(g_metrics_source, "device", device);
        (void)query_source_metric_string(g_metrics_source, "media_type", media_type);
        (void)query_source_metric_string(g_metrics_source, "pixel_type", pixel_type);
        std::string w;
        std::string h;
        if (query_source_metric_string(g_metrics_source, "width", w))
        {
            width = std::atoi(w.c_str());
        }
        if (query_source_metric_string(g_metrics_source, "height", h))
        {
            height = std::atoi(h.c_str());
        }
    }
    if (nullptr != jdec && (media_type == "mjpeg" || pixel_type == "mjpeg"))
    {
        std::string_view decoded;
        if (jdec->query("decoded_pix_fmt", &decoded) == 0 && !decoded.empty())
        {
            pixel_type = std::string(decoded);
        }
    }

    metric_store(*g_pipeline_metrics.get_metric("source.device", g_metric_seed), device);
    metric_store(*g_pipeline_metrics.get_metric("source.width", g_metric_seed),
                 static_cast<int64_t>(width));
    metric_store(*g_pipeline_metrics.get_metric("source.height", g_metric_seed),
                 static_cast<int64_t>(height));
    metric_store(*g_pipeline_metrics.get_metric("source.pixel_type", g_metric_seed), pixel_type);
    metric_store(*g_pipeline_metrics.get_metric("source.media_type", g_metric_seed), media_type);
    metric_store(*g_pipeline_metrics.get_metric("source.out_fps", g_metric_seed), source_out_fps);
    metric_store(*g_pipeline_metrics.get_metric("source.out_kbps", g_metric_seed), source_out_kbps);
    metric_store(*g_pipeline_metrics.get_metric("source.out_bytes", g_metric_seed), source_out_bytes);
    metric_store(*g_pipeline_metrics.get_metric("source.ts", g_metric_seed), ts);
}

void on_signal(int /*sig*/)
{
    g_run = false;
}

void shutdown_pipeline(h264_encoder_t &enc, h264_decoder_mpp &dec, stream_sender &sender,
                       stream_receiver &rcv)
{
    g_run = false;
#if defined(ENABLE_H264_ENCODER_MPP)
    enc.cancel_pending_io();
#endif
    dec.cancel_pending_io();
    sender.close();
    rcv.close();
}

void log_bench_diag(const bench_diag &d, stream_receiver &rcv, stream_sender &sender,
                    h264_encoder_t &enc, const test_app::channel_controller *channel)
{
    std::string_view rcv_stats;
    std::string_view snd_stats;
    std::string_view enc_qp;
    (void)rcv.query("stats", &rcv_stats);
    (void)sender.query("stats", &snd_stats);
    (void)enc.query("qp", &enc_qp);

    const auto ch = (nullptr != channel) ? channel->forward_stats_snapshot()
                                         : test_app::channel_controller::forward_stats {};
    std::fprintf(stderr,
                 "diag: tx noise=%" PRIu64 " jpeg_nv12=%" PRIu64 " nv12=%" PRIu64
                 " enc_err=%" PRIu64 " rtp=%" PRIu64
                 " | rx udp=%" PRIu64 " au=%" PRIu64 " dec_in=%" PRIu64
                 "(eagain=%" PRIu64 " err=%" PRIu64 ") dec_out(eagain=%" PRIu64 " err=%" PRIu64
                 ") nv12=%" PRIu64
                 " present_ok=%" PRIu64 " present_err=%" PRIu64 " present_q_drop=%" PRIu64
                 " au_q_drop=%" PRIu64
                 " | ch in=%" PRIu64 " out=%" PRIu64 " drop_r=%" PRIu64 " drop_l=%" PRIu64
                 " | rcv{%.*s} snd{%.*s} qp=%.*s\n",
                 d.tx_noise.load(), d.tx_jpeg_nv12.load(), d.tx_nv12.load(),
                 d.tx_enc_in_err.load(), d.tx_rtp_sock.load(), d.rx_udp.load(),
                 d.rx_depay_au.load(), d.rx_dec_in_ok.load(), d.rx_dec_in_eagain.load(),
                 d.rx_dec_in_err.load(), d.rx_dec_out_eagain.load(), d.rx_dec_out_err.load(),
                 d.rx_nv12_out.load(), d.rx_present_ok.load(),
                 d.rx_present_err.load(), d.rx_present_q_drop.load(), d.rx_au_q_drop.load(),
                 ch.pkts_in, ch.pkts_out,
                 ch.dropped_rate,
                 ch.dropped_loss, static_cast<int>(rcv_stats.size()), rcv_stats.data(),
                 static_cast<int>(snd_stats.size()), snd_stats.data(),
                 static_cast<int>(enc_qp.size()), enc_qp.data());
}

struct pipeline_counters
{
    uint64_t tx_noise = 0;
    uint64_t tx_source_bytes = 0;
    uint64_t tx_jpeg_nv12 = 0;
    uint64_t tx_jpeg_nv12_bytes = 0;
    uint64_t tx_mjpeg_q_drop = 0;
    uint64_t tx_nv12_q_drop = 0;
    uint64_t tx_enc_nv12_popped = 0;
    uint64_t tx_nv12 = 0;
    uint64_t tx_enc_input_miss = 0;
    uint64_t tx_enc_out_bytes = 0;
    uint64_t tx_rtp_sock = 0;
    uint64_t rx_udp = 0;
    uint64_t rx_dec_in_ok = 0;
    uint64_t rx_dec_in_bytes = 0;
    uint64_t rx_depay_au = 0;
    uint64_t rx_nv12_out = 0;
    uint64_t rx_nv12_out_bytes = 0;
    uint64_t rx_present_ok = 0;
};

struct pipeline_rate_state
{
    std::chrono::steady_clock::time_point t0 {};
    pipeline_counters                       snap {};
    uint64_t                                snd_pkts = 0;
    uint64_t                                snd_bytes = 0;
    uint64_t                                enc_out_bytes = 0;
    uint64_t                                dec_dropped = 0;
    uint64_t                                sink_dropped = 0;
    uint64_t                                rcv_bytes = 0;
    uint64_t                                ch_bytes_in = 0;
    uint64_t                                ch_bytes_out = 0;
    uint64_t                                ch_pkts_out = 0;
    uint64_t                                ch_fwd_drops = 0;
    bool                                    have_snap = false;
};

[[nodiscard]] pipeline_counters snapshot_counters(const bench_diag &d)
{
    pipeline_counters c;
    c.tx_noise = d.tx_noise.load();
    c.tx_source_bytes = d.tx_source_bytes.load();
    c.tx_jpeg_nv12 = d.tx_jpeg_nv12.load();
    c.tx_jpeg_nv12_bytes = d.tx_jpeg_nv12_bytes.load();
    c.tx_mjpeg_q_drop = d.tx_mjpeg_q_drop.load();
    c.tx_nv12_q_drop = d.tx_nv12_q_drop.load();
    c.tx_enc_nv12_popped = d.tx_enc_nv12_popped.load();
    c.tx_nv12 = d.tx_nv12.load();
    c.tx_enc_input_miss = d.tx_enc_input_miss.load();
    c.tx_enc_out_bytes = d.tx_enc_out_bytes.load();
    c.tx_rtp_sock = d.tx_rtp_sock.load();
    c.rx_udp = d.rx_udp.load();
    c.rx_dec_in_ok = d.rx_dec_in_ok.load();
    c.rx_dec_in_bytes = d.rx_dec_in_bytes.load();
    c.rx_depay_au = d.rx_depay_au.load();
    c.rx_nv12_out = d.rx_nv12_out.load();
    c.rx_nv12_out_bytes = d.rx_nv12_out_bytes.load();
    c.rx_present_ok = d.rx_present_ok.load();
    return c;
}

[[nodiscard]] double elapsed_sec(std::chrono::steady_clock::time_point t0,
                                 std::chrono::steady_clock::time_point t1)
{
    const auto dt = std::chrono::duration_cast<std::chrono::duration<double>>(t1 - t0);
    return dt.count() > 0.0 ? dt.count() : 1.0;
}

[[nodiscard]] double rate_per_sec(uint64_t now, uint64_t prev, double dt_sec)
{
    if (now <= prev)
    {
        return 0.0;
    }
    return static_cast<double>(now - prev) / dt_sec;
}

[[nodiscard]] uint64_t parse_stats_field(std::string_view stats, const char *key)
{
    const std::string prefix = std::string(key) + "=";
    const auto              pos = stats.find(prefix);
    if (pos == std::string_view::npos)
    {
        return 0;
    }
    const char *start = stats.data() + pos + prefix.size();
    char       *end = nullptr;
    return std::strtoull(start, &end, 10);
}

template <typename Comp>
[[nodiscard]] double query_rate_kbps(const Comp &comp, const char *key)
{
    std::string_view v;
    if (comp.query(key, &v) != 0)
    {
        return 0.;
    }
    char buf[32];
    const size_t n = std::min(v.size(), sizeof(buf) - 1);
    std::memcpy(buf, v.data(), n);
    buf[n] = '\0';
    return std::strtod(buf, nullptr);
}

template <typename Comp>
[[nodiscard]] uint64_t query_u64(const Comp &comp, const char *key)
{
    std::string_view v;
    if (comp.query(key, &v) != 0)
    {
        return 0;
    }
    char buf[32];
    const size_t n = std::min(v.size(), sizeof(buf) - 1);
    std::memcpy(buf, v.data(), n);
    buf[n] = '\0';
    char *end = nullptr;
    return std::strtoull(buf, &end, 10);
}

int query_encoder_qp(h264_encoder_t &enc);
int query_encoder_cbr_bps(h264_encoder_t &enc);
int prepare_preview_sink(component_sink *preview, bool kmsdrm, int w, int h);

stream_receiver_counters query_receiver_counters(stream_receiver &rcv);

stream_receiver_counters query_peer_counters(stream_sender &sender);

void publish_receiver_link_stats(stream_sender *sender, stream_receiver *rcv);

void log_bench_rate_line(const pipeline_rate_state &rate, h264_encoder_t &enc,
                         const bench_diag &diag)
{
    if (!rate.have_snap)
    {
        return;
    }
    const uint64_t bytes = diag.tx_enc_out_bytes.load();
    const auto     t_now = std::chrono::steady_clock::now();
    const double   dt = elapsed_sec(rate.t0, t_now);
    if (dt < 0.5 || bytes <= rate.enc_out_bytes)
    {
        return;
    }
    const double enc_out_kbps =
        static_cast<double>(bytes - rate.enc_out_bytes) * 8.0 / dt / 1000.0;
    const int cbr_bps = query_encoder_cbr_bps(enc);
    const int cbr_kbps =
        cbr_bps > 0 ? (cbr_bps + 500) / 1000 : test_app::k_encoder_default_cbr_kbps;
    const int qp = query_encoder_qp(enc);
    std::fprintf(stderr, "bench_metrics: out_kbps=%.0f cbr=%d qp=%d dt=%.1f\n", enc_out_kbps,
                 cbr_kbps, qp >= 0 ? qp : 0, dt);
}

void format_stats_timestamp(char *buf, size_t buflen)
{
    using clock = std::chrono::system_clock;
    const auto now = clock::now();
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    const std::time_t sec = clock::to_time_t(now);
    std::tm           tm_local {};
#if defined(_WIN32)
    localtime_s(&tm_local, &sec);
#else
    localtime_r(&sec, &tm_local);
#endif
    std::snprintf(buf, buflen, "%04d-%02d-%02d %02d:%02d:%02d.%03d", tm_local.tm_year + 1900,
                  tm_local.tm_mon + 1, tm_local.tm_mday, tm_local.tm_hour, tm_local.tm_min,
                  tm_local.tm_sec, static_cast<int>(ms.count()));
}

double query_component_latency_ms(component &c)
{
    std::string_view v;
    if (c.query("latency_ms", &v) != 0 || v.empty())
    {
        return 0.0;
    }
    char buf[64];
    const size_t n = std::min(v.size(), sizeof(buf) - 1);
    std::memcpy(buf, v.data(), n);
    buf[n] = '\0';
    char *end = nullptr;
    const double ms = std::strtod(buf, &end);
    if (end == buf || ms < 0.0)
    {
        return 0.0;
    }
    return ms;
}

void update_pipeline_metrics(const bench_diag &d, h264_encoder_t &enc, stream_sender &sender,
                             stream_receiver &rcv, component_sink *preview, bool kmsdrm,
                             pipeline_rate_state &rate,
                             const test_app::channel_controller *channel,
                             jpeg_decoder_multicore *jdec, bool jpeg_active,
                             h264_decoder_mpp *dec)
{
    const auto t_now = std::chrono::steady_clock::now();
    double     dt = 1.0;
    pipeline_counters prev {};
    uint64_t          prev_snd_pkts = 0;
    uint64_t          prev_snd_bytes = 0;
    uint64_t          prev_enc_out_bytes = 0;
    uint64_t          prev_dec_dropped = 0;
    uint64_t          prev_sink_dropped = 0;
    uint64_t          prev_rcv_bytes = 0;
    uint64_t          prev_ch_bytes_in = 0;
    uint64_t          prev_ch_bytes_out = 0;
    uint64_t          prev_ch_pkts_out = 0;
    uint64_t          prev_ch_fwd_drops = 0;
    if (rate.have_snap)
    {
        prev = rate.snap;
        prev_snd_pkts = rate.snd_pkts;
        prev_snd_bytes = rate.snd_bytes;
        prev_enc_out_bytes = rate.enc_out_bytes;
        prev_dec_dropped = rate.dec_dropped;
        prev_sink_dropped = rate.sink_dropped;
        prev_rcv_bytes = rate.rcv_bytes;
        prev_ch_bytes_in = rate.ch_bytes_in;
        prev_ch_bytes_out = rate.ch_bytes_out;
        prev_ch_pkts_out = rate.ch_pkts_out;
        prev_ch_fwd_drops = rate.ch_fwd_drops;
        dt = elapsed_sec(rate.t0, t_now);
    }
    const pipeline_counters now = snapshot_counters(d);

    std::string_view rcv_stats;
    std::string_view snd_stats;
    (void)rcv.query("stats", &rcv_stats);
    (void)sender.query("stats", &snd_stats);

    const uint64_t rcv_bytes_now = parse_stats_field(rcv_stats, "bytes");
    const uint64_t snd_pkts_now = parse_stats_field(snd_stats, "pkts");
    const uint64_t snd_bytes_now = parse_stats_field(snd_stats, "bytes");

    const auto ch = (nullptr != channel) ? channel->forward_stats_snapshot()
                                         : test_app::channel_controller::forward_stats {};
    publish_receiver_link_stats(&sender, &rcv);
    const stream_receiver_counters rcv_cnt = query_peer_counters(sender);

    const double noise_fps = rate_per_sec(now.tx_noise, prev.tx_noise, dt);
    const double jpeg_out_fps = rate_per_sec(now.tx_jpeg_nv12, prev.tx_jpeg_nv12, dt);
    const double enc_in_fps = rate_per_sec(now.tx_nv12, prev.tx_nv12, dt);
    const double enc_pkt_ps = rate_per_sec(now.tx_rtp_sock, prev.tx_rtp_sock, dt);
    const double snd_pkt_ps = rate_per_sec(snd_pkts_now, prev_snd_pkts, dt);
    const double rx_pkt_ps = rate_per_sec(now.rx_udp, prev.rx_udp, dt);
    const double dec_in_au_pps = rate_per_sec(now.rx_dec_in_ok, prev.rx_dec_in_ok, dt);
    const double depay_au_pps = rate_per_sec(now.rx_depay_au, prev.rx_depay_au, dt);
    const double dec_out_fps = rate_per_sec(now.rx_nv12_out, prev.rx_nv12_out, dt);
    const double present_fps = rate_per_sec(now.rx_present_ok, prev.rx_present_ok, dt);
    const uint64_t sink_dropped_now = d.rx_present_q_drop.load();
    const double   sink_drop_fps = rate_per_sec(sink_dropped_now, prev_sink_dropped, dt);
    const double nv12_gap_fps =
        jpeg_out_fps > enc_in_fps ? jpeg_out_fps - enc_in_fps : 0.0;
    const double source_gap_fps =
        noise_fps > enc_in_fps ? noise_fps - enc_in_fps : 0.0;

    const int qp = query_encoder_qp(enc);
    const int qp_val = qp >= 0 ? qp : 0;
    const int cbr_bps = query_encoder_cbr_bps(enc);
    const uint64_t cbr_kbps =
        cbr_bps >= 0 ? static_cast<uint64_t>((cbr_bps + 500) / 1000) : 0ULL;

    const uint64_t nv12_q_drop = d.tx_nv12_q_drop.load();
    const uint64_t enc_input_miss = d.tx_enc_input_miss.load();
    const uint64_t enc_dropped_frames_total =
        nv12_q_drop + enc_input_miss + d.tx_enc_in_err.load();
    const uint64_t dec_dropped_now =
        d.rx_dec_in_err.load() + d.rx_depay_err.load() + d.rx_au_q_drop.load();
    const double   dec_drop_pps = rate_per_sec(dec_dropped_now, prev_dec_dropped, dt);

    /* Encoder output bitrate (H.264 AU bytes); same unit as h264_encoder.cbr_kbps. */
    const uint64_t enc_out_bytes_now = now.tx_enc_out_bytes;
    double enc_out_kbps = 0.0;
    /* Metrics thread ticks ~250ms; do not require dt>=0.5 (that gate never fired). */
    if (rate.have_snap && dt > 0.0 && enc_out_bytes_now >= prev_enc_out_bytes)
    {
        if (enc_out_bytes_now > prev_enc_out_bytes)
        {
            enc_out_kbps =
                static_cast<double>(enc_out_bytes_now - prev_enc_out_bytes) * 8.0 / dt / 1000.0;
        }
    }
    double rcv_out_kbps = 0.0;
    if (rate.have_snap && dt > 0.0 && rcv_bytes_now >= prev_rcv_bytes && rcv_bytes_now > prev_rcv_bytes)
    {
        rcv_out_kbps =
            static_cast<double>(rcv_bytes_now - prev_rcv_bytes) * 8.0 / dt / 1000.0;
    }
    double snd_in_kbps = 0.0;
    if (rate.have_snap && dt > 0.0 && snd_bytes_now >= prev_snd_bytes && snd_bytes_now > prev_snd_bytes)
    {
        snd_in_kbps = static_cast<double>(snd_bytes_now - prev_snd_bytes) * 8.0 / dt / 1000.0;
    }
    double source_out_kbps = 0.0;
    if (rate.have_snap && dt > 0.0 && now.tx_source_bytes >= prev.tx_source_bytes &&
        now.tx_source_bytes > prev.tx_source_bytes)
    {
        source_out_kbps =
            static_cast<double>(now.tx_source_bytes - prev.tx_source_bytes) * 8.0 / dt / 1000.0;
    }
    double jpeg_nv12_out_kbps = 0.0;
    if (rate.have_snap && dt > 0.0 && now.tx_jpeg_nv12_bytes >= prev.tx_jpeg_nv12_bytes &&
        now.tx_jpeg_nv12_bytes > prev.tx_jpeg_nv12_bytes)
    {
        jpeg_nv12_out_kbps =
            static_cast<double>(now.tx_jpeg_nv12_bytes - prev.tx_jpeg_nv12_bytes) * 8.0 / dt /
            1000.0;
    }
    double dec_in_kbps = 0.0;
    if (rate.have_snap && dt > 0.0 && now.rx_dec_in_bytes >= prev.rx_dec_in_bytes &&
        now.rx_dec_in_bytes > prev.rx_dec_in_bytes)
    {
        dec_in_kbps =
            static_cast<double>(now.rx_dec_in_bytes - prev.rx_dec_in_bytes) * 8.0 / dt / 1000.0;
    }
    double dec_nv12_out_kbps = 0.0;
    if (rate.have_snap && dt > 0.0 && now.rx_nv12_out_bytes >= prev.rx_nv12_out_bytes &&
        now.rx_nv12_out_bytes > prev.rx_nv12_out_bytes)
    {
        dec_nv12_out_kbps =
            static_cast<double>(now.rx_nv12_out_bytes - prev.rx_nv12_out_bytes) * 8.0 / dt / 1000.0;
    }
    const double enc_q_pop_fps =
        rate_per_sec(now.tx_enc_nv12_popped, prev.tx_enc_nv12_popped, dt);
    const size_t enc_q_depth = (nullptr != g_metrics_nv12_q) ? g_metrics_nv12_q->size() : 0;
    double       enc_q_latency_ms = 0.0;
    if (jpeg_out_fps > 0.5)
    {
        enc_q_latency_ms = static_cast<double>(enc_q_depth) * 1000.0 / jpeg_out_fps;
    }
    const double ch_fwd_kbps =
        ch.bytes_out > prev_ch_bytes_out
            ? static_cast<double>(ch.bytes_out - prev_ch_bytes_out) * 8.0 / dt / 1000.0
            : 0.0;
    const uint64_t ch_fwd_drops = ch.dropped_rate + ch.dropped_loss;
    const double   ch_drop_pps = rate_per_sec(ch_fwd_drops, prev_ch_fwd_drops, dt);
    double         ch_drop_kbps = 0.0;
    if (rate.have_snap && dt > 0.0 && ch.bytes_in >= prev_ch_bytes_in &&
        ch.bytes_out >= prev_ch_bytes_out)
    {
        const uint64_t din = ch.bytes_in - prev_ch_bytes_in;
        const uint64_t dout = ch.bytes_out - prev_ch_bytes_out;
        if (din > dout)
        {
            ch_drop_kbps = static_cast<double>(din - dout) * 8.0 / dt / 1000.0;
        }
    }

    uint64_t sink_frames = now.rx_present_ok;
    std::string_view sink_stats;
    if (nullptr != preview && preview->query("stats", &sink_stats) == 0)
    {
        sink_frames = parse_stats_field(sink_stats, "frames");
    }

    char ts[40];
    format_stats_timestamp(ts, sizeof(ts));

    const bool pipeline_flowing =
        (now.tx_rtp_sock > 0 && now.rx_udp > 0) || (enc_pkt_ps > 0.5 && rx_pkt_ps > 0.5);

    const double snd_pps = snd_pkt_ps > 0.0 ? snd_pkt_ps : enc_pkt_ps;
    const uint64_t rx_packets =
        rcv_cnt.fec_packet_received > 0 ? rcv_cnt.fec_packet_received : now.rx_udp;

    metric_store(*g_pipeline_metrics.get_metric("stream_sdl.status", g_metric_seed),
                 "running");
    metric_store(*g_pipeline_metrics.get_metric("stream_sdl.display", g_metric_seed),
                 kmsdrm ? "kmsdrm" : "sdl");
    if (kmsdrm)
    {
        const char *note = present_fps > 0.5 ? "kmsdrm presenting decoded frames"
                                             : "kmsdrm active; waiting for decode/present";
        metric_store(*g_pipeline_metrics.get_metric("stream_sdl.note", g_metric_seed), note);
    }
    metric_store(*g_pipeline_metrics.get_metric("stream_sdl.pipeline_ok", g_metric_seed),
                 pipeline_flowing ? "yes" : "warming");

    const double glass_ms = g_glass_latency_ms.load(std::memory_order_relaxed);
    metric_store(*g_pipeline_metrics.get_metric("stream_sdl.glass_latency_ms", g_metric_seed),
                 glass_ms);
    metric_store(*g_pipeline_metrics.get_metric("latency.glass_ms", g_metric_seed), glass_ms);
    metric_store(*g_pipeline_metrics.get_metric("latency.source_ms", g_metric_seed),
                 g_latency_source_ms.load(std::memory_order_relaxed));
    metric_store(*g_pipeline_metrics.get_metric("latency.jpeg_ms", g_metric_seed),
                 g_latency_jpeg_ms.load(std::memory_order_relaxed));
    metric_store(*g_pipeline_metrics.get_metric("latency.enc_in_ms", g_metric_seed),
                 g_latency_enc_in_ms.load(std::memory_order_relaxed));
    metric_store(*g_pipeline_metrics.get_metric("latency.enc_out_ms", g_metric_seed),
                 g_latency_enc_out_ms.load(std::memory_order_relaxed));
    metric_store(*g_pipeline_metrics.get_metric("latency.depay_ms", g_metric_seed),
                 g_latency_depay_ms.load(std::memory_order_relaxed));
    metric_store(*g_pipeline_metrics.get_metric("latency.dec_in_ms", g_metric_seed),
                 g_latency_dec_in_ms.load(std::memory_order_relaxed));
    metric_store(*g_pipeline_metrics.get_metric("latency.dec_out_ms", g_metric_seed),
                 g_latency_dec_out_ms.load(std::memory_order_relaxed));
    metric_store(*g_pipeline_metrics.get_metric("latency.present_ms", g_metric_seed),
                 g_latency_present_ms.load(std::memory_order_relaxed));

    store_source_pipeline_metrics(noise_fps, source_out_kbps, now.tx_source_bytes, ts,
                                  jpeg_active ? jdec : nullptr);

    if (jpeg_active)
    {
        metric_store(*g_pipeline_metrics.get_metric("jpeg_decoder.status", g_metric_seed), "active");
        metric_store(*g_pipeline_metrics.get_metric("jpeg_decoder.in_fps", g_metric_seed), noise_fps);
        metric_store(*g_pipeline_metrics.get_metric("jpeg_decoder.in_frames", g_metric_seed),
                     now.tx_noise);
        metric_store(*g_pipeline_metrics.get_metric("jpeg_decoder.out_fps", g_metric_seed),
                     jpeg_out_fps);
        metric_store(*g_pipeline_metrics.get_metric("jpeg_decoder.out_frames", g_metric_seed),
                     now.tx_jpeg_nv12);
        metric_store(*g_pipeline_metrics.get_metric("jpeg_decoder.out_kbps", g_metric_seed),
                     jpeg_nv12_out_kbps);
        metric_store(*g_pipeline_metrics.get_metric("jpeg_decoder.out_bytes", g_metric_seed),
                     now.tx_jpeg_nv12_bytes);
        const uint64_t mjpeg_q_drop = d.tx_mjpeg_q_drop.load();
        metric_store(*g_pipeline_metrics.get_metric("jpeg_decoder.dropped_frames", g_metric_seed),
                     mjpeg_q_drop);
        metric_store(*g_pipeline_metrics.get_metric("jpeg_decoder.dropped_fps", g_metric_seed),
                     rate_per_sec(mjpeg_q_drop, prev.tx_mjpeg_q_drop, dt));
        metric_store(*g_pipeline_metrics.get_metric("jpeg_decoder.latency_ms", g_metric_seed),
                     g_latency_jpeg_ms.load(std::memory_order_relaxed));
        if (nullptr != jdec)
        {
            std::string_view sz;
            if (jdec->query("size", &sz) == 0)
            {
                metric_store(*g_pipeline_metrics.get_metric("jpeg_decoder.size", g_metric_seed),
                             std::string(sz));
            }
            std::string_view workers;
            if (jdec->query("workers", &workers) == 0)
            {
                metric_store(*g_pipeline_metrics.get_metric("jpeg_decoder.workers", g_metric_seed),
                             std::string(workers));
            }
        }
    }
    else
    {
        metric_store(*g_pipeline_metrics.get_metric("jpeg_decoder.status", g_metric_seed), "bypass");
        metric_store(*g_pipeline_metrics.get_metric("jpeg_decoder.out_fps", g_metric_seed), 0.0);
        metric_store(*g_pipeline_metrics.get_metric("jpeg_decoder.out_frames", g_metric_seed),
                     now.tx_jpeg_nv12);
    }

    metric_store(*g_pipeline_metrics.get_metric("encoder_queue.in_fps", g_metric_seed), jpeg_out_fps);
    metric_store(*g_pipeline_metrics.get_metric("encoder_queue.in_frames", g_metric_seed),
                 now.tx_jpeg_nv12);
    metric_store(*g_pipeline_metrics.get_metric("encoder_queue.out_fps", g_metric_seed), enc_q_pop_fps);
    metric_store(*g_pipeline_metrics.get_metric("encoder_queue.out_frames", g_metric_seed),
                 now.tx_enc_nv12_popped);
    metric_store(*g_pipeline_metrics.get_metric("encoder_queue.size", g_metric_seed),
                 static_cast<int64_t>(enc_q_depth));
    metric_store(*g_pipeline_metrics.get_metric("encoder_queue.latency_ms", g_metric_seed),
                 enc_q_latency_ms);

    metric_store(*g_pipeline_metrics.get_metric("h264_encoder.in_fps", g_metric_seed), enc_in_fps);
    metric_store(*g_pipeline_metrics.get_metric("h264_encoder.source_gap_fps", g_metric_seed),
                 source_gap_fps);
    metric_store(*g_pipeline_metrics.get_metric("h264_encoder.in_frames", g_metric_seed),
                 now.tx_nv12);
    metric_store(*g_pipeline_metrics.get_metric("h264_encoder.dropped_fps", g_metric_seed),
                 nv12_gap_fps);
    metric_store(*g_pipeline_metrics.get_metric("h264_encoder.dropped_frames", g_metric_seed),
                 enc_dropped_frames_total);
    metric_store(*g_pipeline_metrics.get_metric("h264_encoder.out_pps", g_metric_seed), enc_pkt_ps);
    metric_store(*g_pipeline_metrics.get_metric("h264_encoder.out_packets", g_metric_seed),
                 now.tx_rtp_sock);
    metric_store(*g_pipeline_metrics.get_metric("h264_encoder.cbr_kbps", g_metric_seed), cbr_kbps);
    metric_store(*g_pipeline_metrics.get_metric("h264_encoder.qp", g_metric_seed),
                 static_cast<int64_t>(qp_val));
    metric_store(*g_pipeline_metrics.get_metric("h264_encoder.out_kbps", g_metric_seed), enc_out_kbps);
    metric_store(*g_pipeline_metrics.get_metric("h264_encoder.out_bytes", g_metric_seed),
                 enc_out_bytes_now);
    metric_store(*g_pipeline_metrics.get_metric("h264_encoder.latency_ms", g_metric_seed),
                 query_component_latency_ms(enc));

    metric_store(*g_pipeline_metrics.get_metric("stream_sender.in_pps", g_metric_seed), snd_pps);
    metric_store(*g_pipeline_metrics.get_metric("stream_sender.in_packets", g_metric_seed),
                 snd_pkts_now);
    metric_store(*g_pipeline_metrics.get_metric("stream_sender.in_kbps", g_metric_seed), snd_in_kbps);
    metric_store(*g_pipeline_metrics.get_metric("stream_sender.in_bytes", g_metric_seed), snd_bytes_now);
    metric_store(*g_pipeline_metrics.get_metric("stream_sender.fec_k", g_metric_seed),
                 static_cast<int64_t>(query_u64(sender, "fec_k")));
    metric_store(*g_pipeline_metrics.get_metric("stream_sender.fec_n", g_metric_seed),
                 static_cast<int64_t>(query_u64(sender, "fec_n")));
    const uint64_t fec_recovered = query_u64(rcv, "fec_recovered");
    const uint64_t fec_failures = query_u64(rcv, "fec_failures");
    metric_store(*g_pipeline_metrics.get_metric("stream_sender.fec_recovered", g_metric_seed),
                 fec_recovered);
    metric_store(*g_pipeline_metrics.get_metric("stream_sender.fec_failures", g_metric_seed),
                 fec_failures);
    metric_store(*g_pipeline_metrics.get_metric("stream_sender.fec_oversized", g_metric_seed),
                 query_u64(sender, "fec_oversized"));

    metric_store(*g_pipeline_metrics.get_metric("channel.forward_kbps", g_metric_seed), ch_fwd_kbps);
    metric_store(*g_pipeline_metrics.get_metric("channel.forward_bytes", g_metric_seed),
                 ch.bytes_out);
    metric_store(*g_pipeline_metrics.get_metric("channel.dropped_pps", g_metric_seed), ch_drop_pps);
    metric_store(*g_pipeline_metrics.get_metric("channel.dropped_kbps", g_metric_seed), ch_drop_kbps);
    const double ch_max_kbps = (nullptr != channel) ? channel->max_kbps() : 0.0;
    const double ch_constant_loss_pct =
        (nullptr != channel) ? channel->constant_loss() : 0.0;
    metric_store(*g_pipeline_metrics.get_metric("channel.max_kbps", g_metric_seed), ch_max_kbps);
    metric_store(*g_pipeline_metrics.get_metric("channel.constant_loss_pct", g_metric_seed),
                 ch_constant_loss_pct);

    metric_store(*g_pipeline_metrics.get_metric("stream_receiver.in_pps", g_metric_seed), rx_pkt_ps);
    metric_store(*g_pipeline_metrics.get_metric("stream_receiver.out_kbps", g_metric_seed), rcv_out_kbps);
    metric_store(*g_pipeline_metrics.get_metric("stream_receiver.out_bytes", g_metric_seed),
                 rcv_bytes_now);
    metric_store(*g_pipeline_metrics.get_metric("stream_receiver.fec_recovered", g_metric_seed),
                 fec_recovered);
    metric_store(*g_pipeline_metrics.get_metric("stream_receiver.fec_failures", g_metric_seed),
                 fec_failures);
    metric_store(*g_pipeline_metrics.get_metric("stream_receiver.in_packets", g_metric_seed),
                 rx_packets);

    metric_store(*g_pipeline_metrics.get_metric("h264_decoder.in_pps", g_metric_seed),
                 dec_in_au_pps);
    metric_store(*g_pipeline_metrics.get_metric("h264_decoder.in_packets", g_metric_seed),
                 now.rx_dec_in_ok);
    metric_store(*g_pipeline_metrics.get_metric("h264_decoder.in_kbps", g_metric_seed), dec_in_kbps);
    metric_store(*g_pipeline_metrics.get_metric("h264_decoder.in_bytes", g_metric_seed),
                 now.rx_dec_in_bytes);
    metric_store(*g_pipeline_metrics.get_metric("h264_decoder.depay_au_pps", g_metric_seed),
                 depay_au_pps);
    metric_store(*g_pipeline_metrics.get_metric("h264_decoder.dropped_pps", g_metric_seed),
                 dec_drop_pps);
    metric_store(*g_pipeline_metrics.get_metric("h264_decoder.dropped_packets", g_metric_seed),
                 dec_dropped_now);
    metric_store(*g_pipeline_metrics.get_metric("h264_decoder.out_fps", g_metric_seed), dec_out_fps);
    metric_store(*g_pipeline_metrics.get_metric("h264_decoder.out_frames", g_metric_seed),
                 now.rx_nv12_out);
    metric_store(*g_pipeline_metrics.get_metric("h264_decoder.out_kbps", g_metric_seed),
                 dec_nv12_out_kbps);
    metric_store(*g_pipeline_metrics.get_metric("h264_decoder.out_bytes", g_metric_seed),
                 now.rx_nv12_out_bytes);
    if (nullptr != dec)
    {
        metric_store(*g_pipeline_metrics.get_metric("h264_decoder.latency_ms", g_metric_seed),
                     query_component_latency_ms(*dec));
    }
    else
    {
        metric_store(*g_pipeline_metrics.get_metric("h264_decoder.latency_ms", g_metric_seed),
                     g_latency_dec_out_ms.load(std::memory_order_relaxed));
    }

    metric_store(*g_pipeline_metrics.get_metric("sdl_sink.in_fps", g_metric_seed), present_fps);
    metric_store(*g_pipeline_metrics.get_metric("sdl_sink.in_frames", g_metric_seed), sink_frames);
    metric_store(*g_pipeline_metrics.get_metric("sdl_sink.dropped_fps", g_metric_seed),
                 sink_drop_fps);
    metric_store(*g_pipeline_metrics.get_metric("sdl_sink.dropped_frames", g_metric_seed),
                 sink_dropped_now);
    metric_store(*g_pipeline_metrics.get_metric("sdl_sink.render_fps", g_metric_seed), present_fps);
    metric_store(*g_pipeline_metrics.get_metric("sdl_sink.latency_ms", g_metric_seed), glass_ms);

    rate.snap = now;
    rate.t0 = t_now;
    rate.snd_pkts = snd_pkts_now;
    rate.snd_bytes = snd_bytes_now;
    rate.enc_out_bytes = enc_out_bytes_now;
    rate.dec_dropped = dec_dropped_now;
    rate.sink_dropped = sink_dropped_now;
    rate.rcv_bytes = rcv_bytes_now;
    rate.ch_bytes_in = ch.bytes_in;
    rate.ch_bytes_out = ch.bytes_out;
    rate.ch_pkts_out = ch.pkts_out;
    rate.ch_fwd_drops = ch_fwd_drops;
    rate.have_snap = true;
}

int cfg_str(component &c, const char *key, const char *val)
{
    std::string_view k(key);
    std::string_view v(val);
    return c.configure(k, &v);
}

int open_stage(const char *name, int rc)
{
    if (rc < 0)
    {
        std::fprintf(stderr, "open failed: %s (%d", name, rc);
        if (-rc > 0 && -rc < 4096)
        {
            std::fprintf(stderr, "; %s", std::strerror(-rc));
        }
        std::fprintf(stderr, ")\n");
    }
    return rc;
}

bool forward_encoded_au(rtp_h264_pay &pay, stream_sender &sender, bench_diag &diag,
                        data_packet &pkt)
{
    log_stage_latency("enc_out", pkt);
    if (pay.input(0, pkt) < 0)
    {
        return false;
    }
    bool sent = false;
    data_packet sock_pkt;
    while (g_run.load() && pay.output(0, sock_pkt, 0) == 0)
    {
        if (sender.input(0, sock_pkt) == 0)
        {
            diag.tx_rtp_sock++;
            sent = true;
        }
    }
    return sent;
}

void forward_aus_and_note_emitted(rtp_h264_pay &pay, stream_sender &sender, bench_diag &diag,
                                  std::vector<data_packet> &aus)
{
    for (data_packet &pkt : aus)
    {
        const frame_data &f = data_packet::cast<frame_data>(pkt);
        diag.tx_enc_out_bytes.fetch_add(f.buf.size, std::memory_order_relaxed);
        (void)forward_encoded_au(pay, sender, diag, pkt);
    }
}

void pull_encoded_aus_unlocked(h264_encoder_t &enc, std::vector<data_packet> &out)
{
    data_packet pkt;
    while (g_run.load() && enc.output(0, pkt, 0) == 0)
    {
        out.push_back(std::move(pkt));
    }
}

void pull_encoded_aus_locked(h264_encoder_t &enc, std::vector<data_packet> &out)
{
    /* Encoder has its own MPP context; do not serialize with RX decode on g_mpp_hw_mu. */
    pull_encoded_aus_unlocked(enc, out);
}

void pull_encoded_aus(h264_encoder_t &enc, std::vector<data_packet> &out)
{
    pull_encoded_aus_locked(enc, out);
}

void drain_encoder(h264_encoder_t &enc, rtp_h264_pay &pay, stream_sender &sender, bench_diag &diag)
{
    std::vector<data_packet> aus;
    pull_encoded_aus_locked(enc, aus);
    forward_aus_and_note_emitted(pay, sender, diag, aus);
}

bool submit_nv12_to_encoder(h264_encoder_t *enc, rtp_h264_pay *pay, stream_sender *sender,
                            bench_diag *diag, data_packet &nv12)
{
    std::vector<data_packet> enc_pending;
    bool                     got_enc_in = false;
    for (int attempt = 0; g_run.load() && attempt < 48; attempt++)
    {
        const int enc_in = enc->input(0, nv12);
        if (0 == enc_in)
        {
            diag->tx_nv12++;
            got_enc_in = true;
            log_stage_latency("enc_in", nv12);
            break;
        }
        if (-ECANCELED == enc_in)
        {
            return false;
        }
        if (-EAGAIN == enc_in)
        {
            pull_encoded_aus(*enc, enc_pending);
            continue;
        }
        diag->tx_enc_in_err++;
        const uint64_t n = diag->tx_enc_in_err.load();
        if (n <= 8)
        {
            std::fprintf(stderr, "stream_sdl: h264_encoder input failed (%d", enc_in);
            if (-enc_in > 0 && -enc_in < 4096)
            {
                std::fprintf(stderr, "; %s", std::strerror(-enc_in));
            }
            std::fprintf(stderr, ")\n");
        }
        break;
    }
    if (!got_enc_in)
    {
        diag->tx_enc_input_miss++;
    }
    pull_encoded_aus(*enc, enc_pending);
    forward_aus_and_note_emitted(*pay, *sender, *diag, enc_pending);
    return true;
}

void source_stage_main(component_source *source, pipeline_queue *mjpeg_q, pipeline_queue *nv12_q,
                       bool direct_nv12, bench_diag *diag)
{
    pin_current_thread_to_cpu(k_cpu_noise);
    while (g_run.load())
    {
        data_packet raw;
        const int got = source->output(0, raw, g_run.load() ? -1 : 0);
        if (got < 0)
        {
            if (-EBADF == got || -ECANCELED == got)
            {
                break;
            }
            if (-EAGAIN == got)
            {
                continue;
            }
            std::fprintf(stderr, "stream_sdl: source output failed (%d", got);
            if (-got > 0 && -got < 4096)
            {
                std::fprintf(stderr, "; %s", std::strerror(-got));
            }
            std::fprintf(stderr, ")\n");
            break;
        }
        diag->tx_noise++;
        diag->tx_source_bytes += packet_frame_bytes(raw);
        note_source_pts(raw);
        log_stage_latency("source", raw);
        if (direct_nv12)
        {
            diag->tx_jpeg_nv12++;
            diag->tx_jpeg_nv12_bytes += packet_frame_bytes(raw);
            if (!nv12_q->push(std::move(raw), &diag->tx_nv12_q_drop))
            {
                break;
            }
        }
        else if (!mjpeg_q->push(std::move(raw), &diag->tx_mjpeg_q_drop))
        {
            break;
        }
    }
}

void jpeg_stage_main(jpeg_decoder_multicore *jdec, pipeline_queue *mjpeg_q, pipeline_queue *nv12_q,
                     bench_diag *diag)
{
    pin_current_thread_to_cpu(k_cpu_jpeg);
    while (g_run.load())
    {
        data_packet raw;
        if (!mjpeg_q->pop(raw, 50))
        {
            continue;
        }

        bool submitted = false;
        for (int attempt = 0; g_run.load() && !submitted; attempt++)
        {
            const int ir = jdec->input(0, raw);
            if (0 == ir)
            {
                submitted = true;
                break;
            }
            if (-EBADF == ir || -ECANCELED == ir)
            {
                return;
            }
            if (-EAGAIN != ir)
            {
                std::fprintf(stderr, "stream_sdl: jpeg_decoder input failed (%d)\n", ir);
                break;
            }
            if (attempt >= 64)
            {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!submitted)
        {
            continue;
        }

        data_packet nv12;
        int         or_out = 0;
        while (g_run.load())
        {
            or_out = jdec->output(0, nv12, -1);
            if (0 == or_out)
            {
                break;
            }
            if (-EBADF == or_out || -ECANCELED == or_out)
            {
                return;
            }
            if (-EAGAIN == or_out)
            {
                break;
            }
            std::fprintf(stderr, "stream_sdl: jpeg_decoder output failed (%d)\n", or_out);
            break;
        }
        if (0 != or_out)
        {
            continue;
        }
        diag->tx_jpeg_nv12++;
        diag->tx_jpeg_nv12_bytes += packet_frame_bytes(nv12);
        log_stage_latency("jpeg_nv12", nv12);
        (void)nv12_q->push(std::move(nv12), &diag->tx_nv12_q_drop);
    }
}

void apply_pending_console_encoder_cfg(h264_encoder_t &enc)
{
    const int kbps = g_pending_console_cbr_kbps.exchange(-1, std::memory_order_acq_rel);
    if (kbps >= 100)
    {
        char bps_buf[32];
        std::snprintf(bps_buf, sizeof(bps_buf), "%d", kbps * 1000);
        std::string_view val = bps_buf;
        (void)enc.configure("cbr", &val);
    }
    const int qp = g_pending_console_qp.exchange(-1, std::memory_order_acq_rel);
    if (qp >= 0 && qp <= 51)
    {
        char qp_buf[16];
        std::snprintf(qp_buf, sizeof(qp_buf), "%d", qp);
        std::string_view val = qp_buf;
        (void)enc.configure("qp", &val);
    }
    const int gop = g_pending_console_gop.exchange(-1, std::memory_order_acq_rel);
    if (gop >= 1 && gop <= 255)
    {
        char gop_buf[16];
        std::snprintf(gop_buf, sizeof(gop_buf), "%d", gop);
        std::string_view val = gop_buf;
        (void)enc.configure("gop", &val);
    }
}

void encode_stage_main(h264_encoder_t *enc, rtp_h264_pay *pay, stream_sender *sender,
                       pipeline_queue *nv12_q, bench_diag *diag)
{
    pin_current_thread_to_cpu(k_cpu_encode);
    while (g_run.load())
    {
        apply_pending_console_encoder_cfg(*enc);
        data_packet nv12;
        if (!nv12_q->pop(nv12, 50))
        {
            drain_encoder(*enc, *pay, *sender, *diag);
            continue;
        }
        diag->tx_enc_nv12_popped++;
        if (!submit_nv12_to_encoder(enc, pay, sender, diag, nv12))
        {
            break;
        }
    }
}

int drain_decoder_one_frame(h264_decoder_mpp *dec, data_packet &frame_pkt, bench_diag &diag,
                            int timeout_ms)
{
    const int r = dec->output(0, frame_pkt, timeout_ms);
    if (0 == r)
    {
        diag.rx_nv12_out++;
        diag.rx_nv12_out_bytes += packet_frame_bytes(frame_pkt);
        return 0;
    }
    if (-EAGAIN == r)
    {
        diag.rx_dec_out_eagain++;
        return r;
    }
    diag.rx_dec_out_err++;
    if (diag.rx_dec_out_err <= 10)
    {
        std::fprintf(stderr, "stream_sdl: h264_decoder output failed (%d", r);
        if (-r > 0 && -r < 4096)
        {
            std::fprintf(stderr, "; %s", std::strerror(-r));
        }
        std::fprintf(stderr, ")\n");
    }
    return r;
}

void pull_decoder_frames_locked(h264_decoder_mpp *dec, bench_diag &diag,
                                std::vector<data_packet> &out)
{
    std::lock_guard<std::mutex> hw(g_mpp_hw_mu);
    for (int pass = 0; pass < 64; pass++)
    {
        data_packet frame_pkt;
        const int   r = drain_decoder_one_frame(dec, frame_pkt, diag, 0);
        if (0 != r)
        {
            break;
        }
        out.push_back(std::move(frame_pkt));
    }
}

void enqueue_decoded_frames(present_frame_queue *present_q, bench_diag &diag,
                            std::vector<data_packet> &frames)
{
    for (data_packet &frame_pkt : frames)
    {
        if (nullptr == present_q)
        {
            continue;
        }
        log_stage_latency("dec_out", frame_pkt);
        (void)present_q->push(std::move(frame_pkt), &diag.rx_present_q_drop);
    }
}

void drain_decoder_to_present(h264_decoder_mpp *dec, present_frame_queue *present_q,
                              bench_diag &diag)
{
    if (ensure_decoder_open(dec) < 0)
    {
        return;
    }
    std::vector<data_packet> frames;
    pull_decoder_frames_locked(dec, diag, frames);
    enqueue_decoded_frames(present_q, diag, frames);
}

void present_thread_main(component_sink *display, present_frame_queue *present_q, int width,
                         int height, bool kmsdrm, bool sdl_open_on_thread, bench_diag *diag)
{
    if (nullptr == display || nullptr == present_q || nullptr == diag)
    {
        return;
    }
    if (sdl_open_on_thread)
    {
        if (display->open() < 0)
        {
            std::fprintf(stderr, "stream_sdl: SDL display open on present thread failed\n");
            return;
        }
        if (prepare_preview_sink(display, kmsdrm, width, height) < 0)
        {
            std::fprintf(stderr,
                         "stream_sdl: SDL display prepare on present thread failed "
                         "(continuing)\n");
        }
    }
    else if (kmsdrm && prepare_preview_sink(display, kmsdrm, width, height) < 0)
    {
        std::fprintf(stderr,
                     "stream_sdl: kmsdrm prepare on present thread failed (continuing)\n");
    }

    while (g_run.load())
    {
        data_packet frame_pkt;
        if (!present_q->pop(frame_pkt, 50))
        {
            continue;
        }
        const int pr = display->input(0, frame_pkt);
        if (0 == pr)
        {
            diag->rx_present_ok++;
            log_stage_latency("present", frame_pkt);
            if (frame_pkt.get_type() == packet_kind_e::FRAME)
            {
                const frame_data &f = data_packet::cast<frame_data>(frame_pkt);
                if (f.capture_mono_ns <= 0)
                {
                    const int64_t latest = g_latest_source_pts.load(std::memory_order_relaxed);
                    const int   fps = g_stream_fps.load(std::memory_order_relaxed);
                    if (fps > 0 && latest >= f.pts)
                    {
                        const double lag_ms = static_cast<double>(latest - f.pts) * 1000.0 /
                                                static_cast<double>(fps);
                        g_glass_latency_ms.store(lag_ms, std::memory_order_relaxed);
                        g_latency_present_ms.store(lag_ms, std::memory_order_relaxed);
                    }
                }
            }
        }
        else
        {
            diag->rx_present_err++;
            const uint64_t n = diag->rx_present_err.load();
            if (n <= 12)
            {
                std::fprintf(stderr, "stream_sdl: display present failed (%d", pr);
                if (-pr > 0 && -pr < 4096)
                {
                    std::fprintf(stderr, "; %s", std::strerror(-pr));
                }
                std::string_view sink_stats;
                if (display->query(std::string_view("stats"), &sink_stats) == 0)
                {
                    std::fprintf(stderr, "; sink{%.*s}", static_cast<int>(sink_stats.size()),
                                 sink_stats.data());
                }
                std::fprintf(stderr, ")\n");
            }
        }
    }

    data_packet tail;
    while (present_q->pop(tail, 0))
    {
        if (display->input(0, tail) == 0)
        {
            diag->rx_present_ok++;
        }
    }
}

int feed_decoder_au(h264_decoder_mpp *dec, data_packet &au, present_frame_queue *present_q,
                    bench_diag &diag)
{
    if (!g_run.load())
    {
        return -ECANCELED;
    }
    if (ensure_decoder_open(dec) < 0)
    {
        return -EINVAL;
    }
    for (int attempt = 0; g_run.load() && attempt < 48; attempt++)
    {
        int r = 0;
        {
            std::lock_guard<std::mutex> lock(g_mpp_hw_mu);
            r = dec->input(0, au);
        }
        if (0 == r)
        {
            diag.rx_dec_in_ok++;
            diag.rx_dec_in_bytes += packet_frame_bytes(au);
            return 0;
        }
        if (-ECANCELED == r)
        {
            return -ECANCELED;
        }
        if (-EAGAIN == r)
        {
            diag.rx_dec_in_eagain++;
            if (!g_run.load())
            {
                return -ECANCELED;
            }
            drain_decoder_to_present(dec, present_q, diag);
            continue;
        }
        diag.rx_dec_in_err++;
        if (diag.rx_dec_in_err <= 5)
        {
            const frame_data &f = data_packet::cast<frame_data>(au);
            std::fprintf(stderr,
                         "stream_sdl: h264_decoder input failed (%d) au_bytes=%zu\n", r,
                         f.buf.size);
        }
        return r;
    }
    return -EAGAIN;
}

stream_receiver_counters query_receiver_counters(stream_receiver &rcv)
{
    return rcv.link_counters_snapshot();
}

stream_receiver_counters query_peer_counters(stream_sender &sender)
{
    stream_receiver_counters c;
    c.udp_packet_received = query_u64(sender, "peer_udp_packet_received");
    c.fec_packet_received = query_u64(sender, "peer_fec_packet_received");
    c.udp_gap_count = query_u64(sender, "peer_udp_gap_count");
    c.fec_gap_count = query_u64(sender, "peer_fec_gap_count");
    c.fec_air_shard_received = query_u64(sender, "peer_fec_air_shard_received");
    c.loss_udp_pct = query_rate_kbps(sender, "peer_loss_udp_pct");
    c.loss_fec_pct = query_rate_kbps(sender, "peer_loss_fec_pct");
    return c;
}

constexpr int k_receiver_loss_avg_samples = 5;

struct receiver_loss_tracker
{
    stream_receiver_counters prev {};
    bool                     have_prev = false;
    double                   udp_loss[k_receiver_loss_avg_samples] {};
    double                   fec_loss[k_receiver_loss_avg_samples] {};
    int                      udp_n = 0;
    int                      fec_n = 0;
    double                   loss_udp_pct = 0.;
    double                   loss_fec_pct = 0.;
};

receiver_loss_tracker g_rcv_loss;
std::mutex            g_rcv_loss_mu;

double interval_loss_pct(uint64_t pkts_now, uint64_t gap_now, uint64_t pkts_prev, uint64_t gap_prev)
{
    if (pkts_now < pkts_prev || gap_now < gap_prev)
    {
        return -1.;
    }
    const uint64_t dp = pkts_now - pkts_prev;
    const uint64_t dg = gap_now - gap_prev;
    const uint64_t denom = dp + dg;
    if (denom < 8)
    {
        return -1.;
    }
    return static_cast<double>(dg) / static_cast<double>(denom) * 100.0;
}

void push_loss_sample(double sample, double *buf, int &n, double &avg_out)
{
    if (sample < 0.)
    {
        return;
    }
    if (n < k_receiver_loss_avg_samples)
    {
        buf[n] = sample;
        ++n;
    }
    else
    {
        for (int i = 1; i < k_receiver_loss_avg_samples; ++i)
        {
            buf[i - 1] = buf[i];
        }
        buf[k_receiver_loss_avg_samples - 1] = sample;
    }
    double sum = 0.;
    for (int i = 0; i < n; ++i)
    {
        sum += buf[i];
    }
    avg_out = sum / static_cast<double>(n);
}

void update_receiver_loss_deltas(const stream_receiver_counters &cur, receiver_loss_tracker &tr)
{
    if (tr.have_prev)
    {
        /* Wire (pre-FEC): air shards + stream_sequence gaps. */
        const double wire_inst =
            interval_loss_pct(cur.fec_air_shard_received, cur.udp_gap_count,
                              tr.prev.fec_air_shard_received, tr.prev.udp_gap_count);
        push_loss_sample(wire_inst, tr.udp_loss, tr.udp_n, tr.loss_udp_pct);

        /* Post-FEC output: delivered app packets + undelivered output gaps. */
        const double fec_inst =
            interval_loss_pct(cur.fec_packet_received, cur.fec_gap_count, tr.prev.fec_packet_received,
                              tr.prev.fec_gap_count);
        push_loss_sample(fec_inst, tr.fec_loss, tr.fec_n, tr.loss_fec_pct);
    }
    tr.prev = cur;
    tr.have_prev = true;
}

void store_receiver_link_metrics(const stream_receiver_counters &c)
{
    metric_store(
        *g_pipeline_metrics.get_metric("stream_sender.peer_udp_packet_received", g_metric_seed),
        c.udp_packet_received);
    metric_store(
        *g_pipeline_metrics.get_metric("stream_sender.peer_fec_packet_received", g_metric_seed),
        c.fec_packet_received);
    metric_store(*g_pipeline_metrics.get_metric("stream_sender.peer_udp_gap_count", g_metric_seed),
                 c.udp_gap_count);
    metric_store(*g_pipeline_metrics.get_metric("stream_sender.peer_fec_gap_count", g_metric_seed),
                 c.fec_gap_count);
    metric_store(*g_pipeline_metrics.get_metric("stream_sender.peer_loss_udp_pct", g_metric_seed),
                 c.loss_udp_pct);
    metric_store(*g_pipeline_metrics.get_metric("stream_sender.peer_loss_fec_pct", g_metric_seed),
                 c.loss_fec_pct);
}

void publish_receiver_link_stats(stream_sender *sender, stream_receiver *rcv)
{
    if (nullptr == rcv)
    {
        return;
    }
    stream_receiver_counters c = query_receiver_counters(*rcv);
    {
        std::lock_guard<std::mutex> lock(g_rcv_loss_mu);
        update_receiver_loss_deltas(c, g_rcv_loss);
        c.loss_udp_pct = g_rcv_loss.loss_udp_pct;
        c.loss_fec_pct = g_rcv_loss.loss_fec_pct;
    }
    if (nullptr != sender)
    {
        sender->set_receiver_counters(c);
    }
    store_receiver_link_metrics(c);
}

void telemetry_thread_main(stream_sender *sender, stream_receiver *rcv)
{
    while (g_run.load())
    {
        if (nullptr != rcv)
        {
            publish_receiver_link_stats(sender, rcv);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

int prepare_preview_sink(component_sink *preview, bool kmsdrm, int w, int h)
{
    if (kmsdrm)
    {
        return static_cast<sdl_kmsdrm_sink *>(preview)->prepare(w, h);
    }
    return static_cast<sdl_sink *>(preview)->prepare(w, h);
}

int query_encoder_qp(h264_encoder_t &enc)
{
    std::string_view key = "qp";
    std::string_view val;
    if (enc.query(key, &val) < 0 || val.empty())
    {
        return -1;
    }
    return std::atoi(val.data());
}

int query_encoder_cbr_bps(h264_encoder_t &enc)
{
    std::string_view key = "cbr";
    std::string_view val;
    if (enc.query(key, &val) < 0 || val.empty())
    {
        return -1;
    }
    return std::atoi(val.data());
}

int send_channel_console(int console_port, const char *line)
{
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        return -errno;
    }
    sockaddr_in dst {};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(static_cast<uint16_t>(console_port));
    if (inet_pton(AF_INET, test_app::k_loopback_host, &dst.sin_addr) != 1)
    {
        close(fd);
        return -EINVAL;
    }
    const size_t n = std::strlen(line);
    const ssize_t sent =
        sendto(fd, line, n, 0, reinterpret_cast<sockaddr *>(&dst), sizeof(dst));
    close(fd);
    if (sent != static_cast<ssize_t>(n))
    {
        return sent < 0 ? static_cast<int>(-errno) : -EIO;
    }
    return 0;
}

bool run_self_test(h264_encoder_t &enc, component_sink *preview, stream_receiver &rcv,
                   stream_sender &sender, int console_port,
                   test_app::channel_controller *channel)
{
    std::this_thread::sleep_for(std::chrono::seconds(5));

    std::string_view stats_key = "stats";
    std::string_view stats_val;
    if (preview->query(stats_key, &stats_val) < 0)
    {
        std::fprintf(stderr, "self-test: display stats query failed\n");
        return false;
    }
    const uint64_t presented = g_bench_diag.rx_present_ok.load();
    const uint64_t nv12 = g_bench_diag.rx_nv12_out.load();
    if (nullptr != std::getenv("DISPLAY") && presented == 0 && nv12 == 0)
    {
        std::fprintf(stderr,
                     "self-test: no decoded/presented frames (sink %.*s diag present_ok=%" PRIu64
                     " nv12_out=%" PRIu64 " rx_udp=%" PRIu64 " au=%" PRIu64 ")\n",
                     static_cast<int>(stats_val.size()), stats_val.data(), presented, nv12,
                     g_bench_diag.rx_udp.load(), g_bench_diag.rx_depay_au.load());
        log_bench_diag(g_bench_diag, rcv, sender, enc, channel);
        return false;
    }

    const int qp_before = query_encoder_qp(enc);
    if (qp_before < 0)
    {
        std::fprintf(stderr, "self-test: could not read encoder qp\n");
        return false;
    }

    if (send_channel_console(console_port, "set_constant_loss 50\n") < 0)
    {
        std::fprintf(stderr, "self-test: channel console command failed\n");
        return false;
    }

    std::this_thread::sleep_for(std::chrono::seconds(5));
    const int qp_after = query_encoder_qp(enc);
    if (qp_after < 0)
    {
        std::fprintf(stderr, "self-test: could not read encoder qp after loss\n");
        return false;
    }

    if (preview->query(stats_key, &stats_val) < 0)
    {
        std::fprintf(stderr, "self-test: display stats query failed (after loss)\n");
        return false;
    }

    const auto ch = (nullptr != channel) ? channel->forward_stats_snapshot()
                                         : test_app::channel_controller::forward_stats {};

    std::fprintf(stderr,
                 "self-test: qp %d -> %d | ch drop_loss=%" PRIu64 " in=%" PRIu64
                 " | display %.*s | present_ok=%" PRIu64 " nv12_out=%" PRIu64 "\n",
                 qp_before, qp_after, ch.dropped_loss, ch.pkts_in,
                 static_cast<int>(stats_val.size()), stats_val.data(),
                 g_bench_diag.rx_present_ok.load(), g_bench_diag.rx_nv12_out.load());

    if (ch.pkts_in < 16 || ch.dropped_loss < 8)
    {
        std::fprintf(stderr, "self-test: channel did not apply forward loss (check console)\n");
        return false;
    }
    if (qp_after > qp_before)
    {
        return true;
    }
    if (ch.dropped_loss >= 8 && qp_before >= 44)
    {
        std::fprintf(stderr, "self-test: qp at ceiling (%d) with channel loss active\n", qp_before);
        return true;
    }
    std::fprintf(stderr, "self-test: expected qp to rise under 50%% channel loss\n");
    return false;
}

void rx_net_thread_main(stream_receiver *rcv, rtp_h264_depay *depay, rx_au_queue *au_q,
                        bench_diag *diag)
{
    auto depay_sock = [&](data_packet &sock_pkt) {
        diag->rx_udp++;
        const int dep = depay->input(0, sock_pkt);
        if (dep < 0)
        {
            diag->rx_depay_err++;
            if (diag->rx_depay_err <= 5)
            {
                std::fprintf(stderr, "stream_sdl: rtp depay input failed (%d)\n", dep);
            }
            return;
        }
        data_packet au;
        while (g_run.load() && depay->output(0, au, 0) == 0)
        {
            diag->rx_depay_au++;
            if (g_skip_decode.load())
            {
                continue;
            }
            log_stage_latency("depay", au);
            au_q->push(std::move(au), *diag);
        }
    };

    while (g_run.load())
    {
        data_packet sock_pkt;
        const int   got = rcv->output(0, sock_pkt, 50);
        if (0 == got)
        {
            depay_sock(sock_pkt);
            continue;
        }
        if (got < 0 && -EAGAIN != got)
        {
            break;
        }
    }
}

void decode_thread_main(h264_decoder_mpp *dec, present_frame_queue *present_q, rx_au_queue *au_q,
                        bench_diag *diag)
{
    pin_current_thread_to_cpu(k_cpu_rx);

    while (g_run.load())
    {
        if (g_skip_decode.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        data_packet au;
        if (!au_q->pop(au, 50))
        {
            drain_decoder_to_present(dec, present_q, *diag);
            continue;
        }

        log_stage_latency("dec_in", au);
        const int r = feed_decoder_au(dec, au, present_q, *diag);
        if (0 == r)
        {
            drain_decoder_to_present(dec, present_q, *diag);
            continue;
        }
        if (-EAGAIN == r)
        {
            au_q->push(std::move(au), *diag);
            drain_decoder_to_present(dec, present_q, *diag);
            continue;
        }
        if (-ECANCELED == r)
        {
            break;
        }
    }

    drain_decoder_to_present(dec, present_q, *diag);
}

void print_usage(const char *prog)
{
    using test_app::k_chan_console;
    using test_app::k_chan_fwd_ingress;
    using test_app::k_chan_rev_egress;
    using test_app::k_chan_rev_ingress;
    using test_app::k_stream_rx_listen;

    std::fprintf(stderr,
                 "Usage: %s [options]\n"
                 "  --size WxH     default 416x240 (rover); 1080p uses direct NV12 snow (30 fps)\n"
                 "  --fps N        default 30\n"
                 "  --chan-in P    channel forward ingress (default %d)\n"
                 "  --chan-rev P   channel reverse ingress (default %d)\n"
                 "  --rev-egress P reverse egress port (default %d)\n"
                 "  --rx-port P    stream_receiver listen (default %d)\n"
                 "  --console P    channel UDP console + metrics (default %d)\n"
                 "  --display MODE sdl (default) or kmsdrm (SDL kmsdrm / DRM)\n"
                 "  --self-test    run ~8s, verify display frames and adaptive qp\n"
                 "  --diag         pipeline counters + per-stage latency (stderr)\n"
                 "                 (or VSTREAMER_LOG_STAGE_LATENCY=1; EVERY=N th frame)\n"
                 "  --cbr KBPS     encoder CBR target kb/s (default %d)\n"
                 "  --gop N        encoder GOP 1..255 (default: same as --fps; or VSTREAMER_ENC_GOP)\n"
                 "  --source ARG   noise (default) or V4L2 device e.g. /dev/video0\n",
                 prog, k_chan_fwd_ingress, k_chan_rev_ingress, k_chan_rev_egress, k_stream_rx_listen,
                 k_chan_console, test_app::k_encoder_default_cbr_kbps);
}

[[nodiscard]] bool source_arg_is_noise(const char *arg)
{
    return nullptr == arg || arg[0] == '\0' || 0 == std::strcmp(arg, "noise") ||
           0 == std::strcmp(arg, "snow");
}

[[nodiscard]] bool source_arg_is_v4l2_device(const char *arg)
{
    return nullptr != arg && arg[0] != '\0' && !source_arg_is_noise(arg) &&
           std::strncmp(arg, "/dev/", 5) == 0;
}

[[nodiscard]] bool display_use_kmsdrm(const char *mode)
{
    return 0 == std::strcmp(mode, "kmsdrm") || 0 == std::strcmp(mode, "sdl_kmsdrm") ||
           0 == std::strcmp(mode, "sdl_kmsdrm_sink") || 0 == std::strcmp(mode, "dmks") ||
           0 == std::strcmp(mode, "sdl_dmks") || 0 == std::strcmp(mode, "sdl_dmks_sink");
}

}  // namespace
}  // namespace vstreamer

int main(int argc, char **argv)
{
    using namespace vstreamer;
    using test_app::k_chan_console;
    using test_app::k_chan_fwd_ingress;
    using test_app::k_chan_rev_egress;
    using test_app::k_chan_rev_ingress;
    using test_app::k_loopback_host;
    using test_app::k_stream_rx_listen;

    int width = 416;
    int height = 240;
    int fps = 30;
    int chan_in = k_chan_fwd_ingress;
    int chan_rev = k_chan_rev_ingress;
    int rev_egress = k_chan_rev_egress;
    int rx_port = k_stream_rx_listen;
    int console_port = k_chan_console;
#if defined(VSTREAMER_APP_UVC_JPEGDEC_KMSDRM)
    const char *display_mode = "kmsdrm";
    const char *source_arg = "/dev/video0";
    const char *app_label = "uvc_jpegdec_kmsdrm";
#else
    const char *display_mode = "sdl";
    const char *source_arg = "noise";
    const char *app_label = "stream_sdl";
#endif
    bool self_test = false;
    bool diag_log = false;
    int  encoder_cbr_kbps = test_app::k_encoder_default_cbr_kbps;
    int  encoder_gop = 0;

    for (int i = 1; i < argc; i++)
    {
        if (0 == std::strcmp(argv[i], "--size") && i + 1 < argc)
        {
            if (std::sscanf(argv[++i], "%dx%d", &width, &height) != 2)
            {
                print_usage(argv[0]);
                return 1;
            }
        }
        else if (0 == std::strcmp(argv[i], "--fps") && i + 1 < argc)
        {
            fps = std::atoi(argv[++i]);
        }
        else if (0 == std::strcmp(argv[i], "--chan-in") && i + 1 < argc)
        {
            chan_in = std::atoi(argv[++i]);
        }
        else if (0 == std::strcmp(argv[i], "--chan-rev") && i + 1 < argc)
        {
            chan_rev = std::atoi(argv[++i]);
        }
        else if (0 == std::strcmp(argv[i], "--rev-egress") && i + 1 < argc)
        {
            rev_egress = std::atoi(argv[++i]);
        }
        else if (0 == std::strcmp(argv[i], "--rx-port") && i + 1 < argc)
        {
            rx_port = std::atoi(argv[++i]);
        }
        else if (0 == std::strcmp(argv[i], "--console") && i + 1 < argc)
        {
            console_port = std::atoi(argv[++i]);
        }
        else if (0 == std::strcmp(argv[i], "--display") && i + 1 < argc)
        {
            display_mode = argv[++i];
            if (!display_use_kmsdrm(display_mode) && 0 != std::strcmp(display_mode, "sdl") &&
                0 != std::strcmp(display_mode, "default") && 0 != std::strcmp(display_mode, "display"))
            {
                std::fprintf(stderr, "unknown --display mode: %s\n", display_mode);
                print_usage(argv[0]);
                return 1;
            }
        }
        else if (0 == std::strcmp(argv[i], "--self-test"))
        {
            self_test = true;
            diag_log = true;
        }
        else if (0 == std::strcmp(argv[i], "--diag"))
        {
            diag_log = true;
        }
        else if (0 == std::strcmp(argv[i], "--cbr") && i + 1 < argc)
        {
            encoder_cbr_kbps = std::atoi(argv[++i]);
            if (encoder_cbr_kbps < 100 || encoder_cbr_kbps > 200'000)
            {
                std::fprintf(stderr, "--cbr must be 100..200000 kb/s\n");
                return 1;
            }
        }
        else if (0 == std::strcmp(argv[i], "--gop") && i + 1 < argc)
        {
            encoder_gop = std::atoi(argv[++i]);
            if (encoder_gop < 1 || encoder_gop > 255)
            {
                std::fprintf(stderr, "--gop must be 1..255\n");
                return 1;
            }
        }
        else if (0 == std::strcmp(argv[i], "--source") && i + 1 < argc)
        {
            source_arg = argv[++i];
            if (!source_arg_is_noise(source_arg) && !source_arg_is_v4l2_device(source_arg))
            {
                std::fprintf(stderr,
                             "unknown --source %s (use noise or a path like /dev/video0)\n",
                             source_arg);
                print_usage(argv[0]);
                return 1;
            }
#ifndef ENABLE_V4L2_SOURCE
            if (!source_arg_is_noise(source_arg))
            {
                std::fprintf(stderr,
                             "stream_sdl: V4L2 capture not built (ENABLE_V4L2_SOURCE=OFF); "
                             "use --source noise\n");
                return 1;
            }
#endif
        }
        else if (0 == std::strcmp(argv[i], "--help"))
        {
            print_usage(argv[0]);
            return 0;
        }
        else
        {
            print_usage(argv[0]);
            return 1;
        }
    }

    if (width < 32 || (width % 32) != 0 || height < 2 || (height % 2) != 0)
    {
        std::fprintf(stderr, "size must be even; cedar builds also need width %% 32\n");
        return 1;
    }
    if (encoder_gop == 0)
    {
        if (const char *gop_env = std::getenv("VSTREAMER_ENC_GOP");
            nullptr != gop_env && gop_env[0] != '\0')
        {
            encoder_gop = std::atoi(gop_env);
            if (encoder_gop < 1 || encoder_gop > 255)
            {
                std::fprintf(stderr, "VSTREAMER_ENC_GOP must be 1..255\n");
                return 1;
            }
        }
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    char size_buf[32];
    char fps_buf[16];
    char qp_buf[8];
    char gop_buf[8];
    char mtu_buf[8];
    std::snprintf(size_buf, sizeof(size_buf), "%dx%d", width, height);
    std::snprintf(fps_buf, sizeof(fps_buf), "%d", fps);
    int default_qp = (width * height >= 1920 * 1080) ? 42 : 36;
    if (const char *qp_env = std::getenv("VSTREAMER_ENC_QP"); nullptr != qp_env && qp_env[0] != '\0')
    {
        default_qp = std::atoi(qp_env);
    }
    std::snprintf(qp_buf, sizeof(qp_buf), "%d", default_qp);
    const int gop_effective = encoder_gop > 0 ? encoder_gop : fps;
    std::snprintf(gop_buf, sizeof(gop_buf), "%d", gop_effective);
    std::snprintf(mtu_buf, sizeof(mtu_buf), "%d", 1400);

    char stream_buf[64];
    char listen_buf[64];
    std::snprintf(stream_buf, sizeof(stream_buf), "%s:%d", k_loopback_host, chan_in);
    std::snprintf(listen_buf, sizeof(listen_buf), "%s:%d", k_loopback_host, rx_port);

    const bool use_v4l2 = source_arg_is_v4l2_device(source_arg);

    noise_source noise;
#ifdef ENABLE_V4L2_SOURCE
    v4l2_source camera;
#endif
    jpeg_decoder_multicore jdec;
    h264_encoder_t enc;
    rtp_h264_pay pay;
    stream_sender sender;
    stream_receiver rcv;
    rtp_h264_depay depay;
    h264_decoder_mpp dec;
    sdl_sink display_sdl;
    sdl_kmsdrm_sink display_kmsdrm;
    component_sink *preview = &display_sdl;
    const bool kmsdrm = display_use_kmsdrm(display_mode);
    if (kmsdrm)
    {
        preview = &display_kmsdrm;
    }
    test_app::channel_controller channel;

    const bool defer_sdl_to_present = !kmsdrm;

    component_source *source = nullptr;
    const char *source_open_label = "noise_source";
    if (use_v4l2)
    {
#ifdef ENABLE_V4L2_SOURCE
        source = &camera;
        source_open_label = "v4l2_source";
        if (cfg_str(camera, "device", source_arg) < 0 || cfg_str(camera, "size", size_buf) < 0 ||
            cfg_str(camera, "fps", fps_buf) < 0 || cfg_str(camera, "format", "mjpeg") < 0)
        {
            return 1;
        }
#else
        std::fprintf(stderr, "stream_sdl: V4L2 not available\n");
        return 1;
#endif
    }
    else
    {
        source = &noise;
        if (cfg_str(noise, "size", size_buf) < 0 || cfg_str(noise, "fps", fps_buf) < 0)
        {
            return 1;
        }
    }
    const bool noise_direct_nv12 = !use_v4l2 && width * height >= 1920 * 1080;
    if (noise_direct_nv12)
    {
        cfg_str(noise, "format", "nv12");
    }
    cfg_str(jdec, "size", size_buf);
    cfg_str(jdec, "fps", fps_buf);
    cfg_str(jdec, "workers", "1");
    cfg_str(jdec, "worker_cpu", "1");
    if (use_v4l2)
    {
        cfg_str(jdec, "output_mode", "convert");
    }
    cfg_str(enc, "size", size_buf);
    cfg_str(enc, "fps", fps_buf);
    cfg_str(enc, "qp", qp_buf);
    cfg_str(enc, "gop", gop_buf);
    cfg_str(pay, "fps", fps_buf);
    cfg_str(depay, "fps", fps_buf);
    cfg_str(pay, "mtu", mtu_buf);
    cfg_str(sender, "stream", stream_buf);
    cfg_str(sender, "mtu", mtu_buf);
    if (const char *pace = std::getenv("VSTREAMER_WIRE_PACE_KBPS");
        nullptr != pace && pace[0] != '\0' && 0 != std::strcmp(pace, "0"))
    {
        if (cfg_str(sender, "max_kbps", pace) < 0)
        {
            return 1;
        }
        std::fprintf(stderr, "%s: UDP wire pace %s kb/s (VSTREAMER_WIRE_PACE_KBPS)\n", app_label,
                     pace);
    }
    cfg_str(rcv, "listen", listen_buf);
    {
        const char *fec = std::getenv("VSTREAMER_FEC");
        const bool  fec_off =
            nullptr != fec &&
            (0 == std::strcmp(fec, "0") || 0 == std::strcmp(fec, "none"));
        if (!fec_off)
        {
            const char *fk = std::getenv("VSTREAMER_FEC_K");
            const char *fn = std::getenv("VSTREAMER_FEC_N");
            if (cfg_str(sender, "fec", "block") < 0)
            {
                return 1;
            }
            if (nullptr != fk && fk[0] != '\0' && cfg_str(sender, "fec_k", fk) < 0)
            {
                return 1;
            }
            if (nullptr != fn && fn[0] != '\0' && cfg_str(sender, "fec_n", fn) < 0)
            {
                return 1;
            }
            std::fprintf(stderr,
                         "%s: stream_sender FEC block k=%s n=%s (set VSTREAMER_FEC=none for "
                         "plain RTP / camera upstream)\n",
                         app_label, nullptr != fk ? fk : "10", nullptr != fn ? fn : "12");
        }
    }
    cfg_str(dec, "size", size_buf);
    cfg_str(dec, "fps", fps_buf);
    cfg_str(*preview, "title", app_label);

    if (nullptr != std::getenv("VSTREAMER_SKIP_DECODE"))
    {
        g_skip_decode = true;
        std::fprintf(stderr, "%s: decode disabled (VSTREAMER_SKIP_DECODE)\n", app_label);
    }
    if (const char *bench = std::getenv("VSTREAMER_BENCH_METRICS");
        nullptr != bench && bench[0] != '\0' && 0 != std::strcmp(bench, "0"))
    {
        g_bench_metrics_log = true;
    }

    char cbr_bps[24];
    std::snprintf(cbr_bps, sizeof(cbr_bps), "%d", encoder_cbr_kbps * 1000);
    const char *enc_rc = std::getenv("VSTREAMER_ENC_RC");
    if (nullptr == enc_rc || enc_rc[0] == '\0')
    {
        enc_rc = "cbr";
    }
    if (open_stage("h264_encoder cbr", cfg_str(enc, "cbr", cbr_bps)) < 0 ||
        open_stage("h264_encoder rc", cfg_str(enc, "rc", enc_rc)) < 0)
    {
        return 1;
    }

    if (open_stage(source_open_label, source->open()) < 0 ||
        (!noise_direct_nv12 && open_stage("jpeg_decoder", jdec.open()) < 0) ||
        open_stage("h264_encoder", enc.open()) < 0 || open_stage("rtp_h264_pay", pay.open()) < 0 ||
        open_stage("stream_sender", sender.open()) < 0 ||
        open_stage("stream_receiver", rcv.open()) < 0 ||
        open_stage("rtp_h264_depay", depay.open()) < 0 ||
        (defer_sdl_to_present ? 0
                              : open_stage(kmsdrm ? "sdl_kmsdrm_sink" : "sdl_sink", preview->open())) <
            0)
    {
        return 1;
    }

    if (open_stage("h264_encoder rc", cfg_str(enc, "rc", enc_rc)) < 0)
    {
        return 1;
    }
    {
        const int reported_bps = query_encoder_cbr_bps(enc);
        std::fprintf(stderr, "%s: MPP rc=%s target %d kbps\n", app_label, enc_rc,
                     reported_bps > 0 ? (reported_bps + 500) / 1000 : encoder_cbr_kbps);
    }
    channel.set_encode_command_handlers(
        [](int kbps) -> bool {
            g_pending_console_cbr_kbps.store(kbps, std::memory_order_release);
            return true;
        },
        [](int qp) -> bool {
            g_pending_console_qp.store(qp, std::memory_order_release);
            return true;
        },
        [](int gop) -> bool {
            g_pending_console_gop.store(gop, std::memory_order_release);
            return true;
        });
    channel.set_stream_sender(&sender);

    sender.set_enabled(true, 0);

    g_metrics_source = source;
    if (!defer_sdl_to_present && prepare_preview_sink(preview, kmsdrm, width, height) < 0)
    {
        std::fprintf(stderr,
                     "stream_sdl: display prepare failed (continuing; first frame creates "
                     "window)\n");
    }
    if (defer_sdl_to_present)
    {
        std::fprintf(stderr,
                     "stream_sdl: SDL window opens on present thread (OpenGL context)\n");
    }

    const int ch_start =
        channel.start(chan_in, k_loopback_host, rx_port, chan_rev, k_loopback_host, rev_egress);
    if (ch_start < 0)
    {
        std::fprintf(stderr, "channel_controller start failed (%d", ch_start);
        if (-ch_start > 0 && -ch_start < 4096)
        {
            std::fprintf(stderr, "; %s", std::strerror(-ch_start));
        }
        std::fprintf(stderr, ")\n");
        return 1;
    }
    {
        double chan_cap = test_app::k_chan_default_max_kbps;
        if (const char *env_cap = std::getenv("VSTREAMER_CHAN_MAX_KBPS"))
        {
            const double v = std::strtod(env_cap, nullptr);
            if (v >= 0.0)
            {
                chan_cap = v;
            }
        }
        if (chan_cap > 0.0)
        {
            channel.set_max_kbps(chan_cap);
            std::fprintf(stderr, "stream_sdl: channel forward cap %.0f kbps\n", chan_cap);
        }
    }
    pipeline_rate_state pipeline_rate;
    channel.set_pipeline_metrics(&g_pipeline_metrics);
    channel.set_pipeline_metrics_refresh([&, jpeg_active = !noise_direct_nv12]() {
        update_pipeline_metrics(g_bench_diag, enc, sender, rcv, preview, kmsdrm, pipeline_rate,
                                &channel, jpeg_active ? &jdec : nullptr, jpeg_active, &dec);
    });
    if (channel.start_console(console_port) < 0)
    {
        std::fprintf(stderr, "channel_controller console failed\n");
        return 1;
    }

    std::fprintf(stderr,
                 "%s: %s @ %d fps | source %s | display %s | channel fwd :%d->:%d "
                 "rev :%d->:%d | console :%d\n",
                 app_label, size_buf, fps, use_v4l2 ? source_arg : "noise",
                 kmsdrm ? "kmsdrm" : "sdl", chan_in, rx_port, chan_rev, rev_egress, console_port);
    if (noise_direct_nv12)
    {
        std::fprintf(stderr,
                     "stream_sdl: note: 1080p uses direct NV12 snow (skips CPU MJPEG "
                     "encode/decode; rover sizes still use MJPEG)\n");
    }

    g_diag = diag_log;
    if (diag_log)
    {
        std::fprintf(stderr,
                     "stream_sdl: diagnostic logging enabled (--diag); stage_latency lines on\n");
    }
    g_stream_fps.store(fps, std::memory_order_relaxed);
    const size_t pipe_q_depth =
        queue_depth_from_env("VSTREAMER_PIPE_QUEUE_DEPTH", k_default_pipe_queue_depth, 64);
    const size_t present_q_depth = queue_depth_from_env("VSTREAMER_PRESENT_QUEUE_DEPTH",
                                                        k_default_present_queue_depth, 16);
    const size_t rx_au_q_depth =
        queue_depth_from_env("VSTREAMER_RX_AU_QUEUE_DEPTH", k_default_rx_au_queue_depth, 256);
    std::fprintf(stderr,
                 "stream_sdl: queue depths pipe=%zu present=%zu rx_au=%zu "
                 "(override: VSTREAMER_*_QUEUE_DEPTH env)\n",
                 pipe_q_depth, present_q_depth, rx_au_q_depth);
    if (const char *adm = std::getenv("VSTREAMER_ENC_ADMISSION");
        nullptr != adm && adm[0] != '\0' && 0 != std::strcmp(adm, "0"))
    {
        std::fprintf(stderr,
                     "stream_sdl: encode admission pacing ON (VSTREAMER_ENC_ADMISSION); "
                     "MPP CBR only, 1080p+\n");
    }
    pipeline_queue       mjpeg_q(pipe_q_depth);
    pipeline_queue       nv12_q(pipe_q_depth);
    g_metrics_nv12_q = &nv12_q;
    present_frame_queue  present_q(present_q_depth);
    rx_au_queue          au_q(rx_au_q_depth);

    std::thread source_thr(source_stage_main, source, &mjpeg_q, &nv12_q, noise_direct_nv12,
                           &g_bench_diag);
    std::thread jpeg_thr;
    if (!noise_direct_nv12)
    {
        jpeg_thr = std::thread(jpeg_stage_main, &jdec, &mjpeg_q, &nv12_q, &g_bench_diag);
    }
    std::thread encode_thr(encode_stage_main, &enc, &pay, &sender, &nv12_q, &g_bench_diag);
    std::thread tel(telemetry_thread_main, &sender, &rcv);
    std::thread metrics_thr([&, jpeg_active = !noise_direct_nv12]() {
        int bench_log_ticks = 0;
        while (g_run.load())
        {
            update_pipeline_metrics(g_bench_diag, enc, sender, rcv, preview, kmsdrm, pipeline_rate,
                                    &channel, jpeg_active ? &jdec : nullptr, jpeg_active, &dec);
            if (g_bench_metrics_log.load())
            {
                ++bench_log_ticks;
                if (bench_log_ticks >= 5)
                {
                    bench_log_ticks = 0;
                    log_bench_rate_line(pipeline_rate, enc, g_bench_diag);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    });
    std::thread present_thr(present_thread_main, preview, &present_q, width, height, kmsdrm,
                            defer_sdl_to_present, &g_bench_diag);
    std::thread rx_net(rx_net_thread_main, &rcv, &depay, &au_q, &g_bench_diag);
    std::thread decode_thr(decode_thread_main, &dec, &present_q, &au_q, &g_bench_diag);

    bool self_test_ok = true;
    if (self_test)
    {
        self_test_ok = run_self_test(enc, preview, rcv, sender, console_port, &channel);
    }
    else
    {
        while (g_run.load())
        {
            if (diag_log)
            {
                log_bench_diag(g_bench_diag, rcv, sender, enc, &channel);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        }
    }

    shutdown_pipeline(enc, dec, sender, rcv);
    present_q.wake();
    au_q.wake();
    source_thr.join();
    if (jpeg_thr.joinable())
    {
        jpeg_thr.join();
    }
    encode_thr.join();
    tel.join();
    metrics_thr.join();
    rx_net.join();
    decode_thr.join();
    present_thr.join();
    channel.stop();
    source->close();
    if (!noise_direct_nv12)
    {
        jdec.close();
    }

    if (diag_log)
    {
        log_bench_diag(g_bench_diag, rcv, sender, enc, &channel);
    }

    preview->close();
    dec.close();
    depay.close();
    pay.close();
    enc.close();

    if (self_test && !self_test_ok)
    {
        return 1;
    }
    return 0;
}
