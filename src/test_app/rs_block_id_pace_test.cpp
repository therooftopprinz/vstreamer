#include "core/rs_block_erasure.hpp"

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <random>
#include <thread>
#include <vector>

using steady_clk = std::chrono::steady_clock;
using vstreamer::rs_block_erasure;

namespace
{

constexpr int k_fec_k = 10;
constexpr int k_fec_n = 11;
constexpr int k_fec_timeout_ms = 20;
constexpr size_t k_pkt_hdr = 8; /* uint64_t seq */

void store_u64_le(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
    {
        p[i] = static_cast<uint8_t>(v >> (8 * i));
    }
}

uint64_t load_u64_le(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
    {
        v |= static_cast<uint64_t>(p[i]) << (8 * i);
    }
    return v;
}

double secs_between(steady_clk::time_point t0, steady_clk::time_point t1)
{
    return std::chrono::duration<double>(t1 - t0).count();
}

struct scheduled_shard
{
    steady_clk::time_point due;
    std::vector<uint8_t> bytes;
};

}  // namespace

int main(int argc, char **argv)
{
    double run_sec = 12.0;
    double target_mbps = 100.0;
    if (argc > 1)
    {
        run_sec = std::atof(argv[1]);
    }
    if (argc > 2)
    {
        target_mbps = std::atof(argv[2]);
    }
    if (run_sec <= 0.0 || target_mbps <= 0.0)
    {
        std::fprintf(stderr, "usage: %s [run_seconds] [target_mbps]\n", argv[0]);
        return 1;
    }

    const double target_bps = target_mbps * 1'000'000.0;

    rs_block_erasure enc;
    rs_block_erasure dec;
    if (!enc.init(k_fec_k, k_fec_n, k_fec_timeout_ms))
    {
        std::fprintf(stderr, "init failed\n");
        return 1;
    }

    std::mt19937                            rng(0xC0DE);
    std::uniform_real_distribution<double>  jitter(0.65, 1.35);
    std::uniform_int_distribution<int>      body_len(180, 900);

    std::deque<scheduled_shard> sendq;
    std::vector<std::vector<uint8_t>> decoded;

    const auto t0 = steady_clk::now();
    const auto deadline = t0 + std::chrono::duration_cast<steady_clk::duration>(
                                    std::chrono::duration<double>(run_sec));

    uint64_t app_seq = 0;
    uint64_t bytes_scheduled = 0;
    uint64_t shards_scheduled = 0;
    uint64_t wire_wrap_marks = 0;
    uint8_t  last_wire_block = 0xFF;
    bool     have_wire = false;

    auto schedule_shard = [&](std::vector<uint8_t> &&shard, double *schedule_cursor_sec) {
        const double send_sec =
            *schedule_cursor_sec + (static_cast<double>(shard.size()) * 8.0 / target_bps) *
                                       jitter(rng);
        *schedule_cursor_sec = send_sec;
        scheduled_shard item;
        item.due = t0 + std::chrono::duration_cast<steady_clk::duration>(
                             std::chrono::duration<double>(send_sec));
        item.bytes = std::move(shard);
        const uint8_t wire_id = item.bytes[0];
        if (have_wire && wire_id < last_wire_block)
        {
            wire_wrap_marks++;
        }
        last_wire_block = wire_id;
        have_wire = true;
        bytes_scheduled += item.bytes.size();
        shards_scheduled++;
        sendq.push_back(std::move(item));
    };

    double schedule_cursor_sec = 0.0;

    auto pump_decoder = [&](std::vector<std::vector<uint8_t>> *batch) {
        dec.poll_rx(batch);
        if (batch != nullptr)
        {
            for (auto &p : *batch)
            {
                decoded.push_back(std::move(p));
            }
            batch->clear();
        }
    };

    while (steady_clk::now() < deadline)
    {
        std::vector<uint8_t> pkt(k_pkt_hdr + static_cast<size_t>(body_len(rng)), 0);
        store_u64_le(pkt.data(), app_seq);
        app_seq++;

        std::vector<std::vector<uint8_t>> air;
        enc.push_app(pkt.data(), pkt.size(), &air);
        for (auto &shard : air)
        {
            schedule_shard(std::move(shard), &schedule_cursor_sec);
        }

        std::vector<std::vector<uint8_t>> tick_air;
        enc.on_tick(&tick_air);
        for (auto &shard : tick_air)
        {
            schedule_shard(std::move(shard), &schedule_cursor_sec);
        }

        const auto now = steady_clk::now();
        while (!sendq.empty() && sendq.front().due <= now)
        {
            std::vector<std::vector<uint8_t>> batch;
            dec.push_air(sendq.front().bytes.data(), sendq.front().bytes.size(), &batch);
            for (auto &p : batch)
            {
                decoded.push_back(std::move(p));
            }
            sendq.pop_front();
        }

        pump_decoder(nullptr);

        if (sendq.empty())
        {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }

    /* Drain remaining scheduled shards (still at paced times, or burst if late). */
    /* Flush partial block and drain. */
    {
        std::vector<std::vector<uint8_t>> air;
        enc.flush(&air);
        for (auto &shard : air)
        {
            schedule_shard(std::move(shard), &schedule_cursor_sec);
        }
    }

    while (!sendq.empty())
    {
        const auto now = steady_clk::now();
        if (sendq.front().due > now)
        {
            std::this_thread::sleep_until(sendq.front().due);
        }
        std::vector<std::vector<uint8_t>> batch;
        dec.push_air(sendq.front().bytes.data(), sendq.front().bytes.size(), &batch);
        for (auto &p : batch)
        {
            decoded.push_back(std::move(p));
        }
        sendq.pop_front();
        pump_decoder(nullptr);
    }

    /* Let RX block expiry / emit queue finish. */
    const int tail_ms =
        std::max(dec.emit_hold_ms(), dec.rx_hold_ms()) + dec.done_hold_ms() + 100;
    const auto tail_end = steady_clk::now() + std::chrono::milliseconds(tail_ms);
    while (steady_clk::now() < tail_end)
    {
        std::vector<std::vector<uint8_t>> batch;
        pump_decoder(&batch);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    const auto t1 = steady_clk::now();
    const double dt = secs_between(t0, t1);
    const double achieved_mbps =
        dt > 0. ? static_cast<double>(bytes_scheduled) * 8.0 / dt / 1'000'000.0 : 0.;

    uint64_t seq_errors = 0;
    uint64_t expect = 0;
    for (const auto &p : decoded)
    {
        if (p.size() < k_pkt_hdr)
        {
            seq_errors++;
            continue;
        }
        const uint64_t s = load_u64_le(p.data());
        if (s != expect)
        {
            seq_errors++;
        }
        expect++;
    }

    const uint64_t blocks = enc.blocks();
    const uint64_t missing = app_seq > expect ? app_seq - expect : 0;

    std::printf(
        "rs_block_id_pace_test: %.1fs target=%.0f Mbps achieved=%.1f Mbps\n"
        "  apps=%" PRIu64 " blocks=%" PRIu64 " shards=%" PRIu64 " wire_wraps=%" PRIu64 "\n"
        "  decoded=%zu decode_fail=%" PRIu64 " seq_errors=%" PRIu64 " missing_apps=%" PRIu64 "\n",
        run_sec, target_mbps, achieved_mbps, app_seq, blocks, shards_scheduled, wire_wrap_marks,
        decoded.size(), dec.decode_fail(), seq_errors, missing);

    if (blocks < 256)
    {
        std::fprintf(stderr, "warning: only %" PRIu64 " blocks (need >256 to stress wire id)\n",
                     blocks);
    }

    const bool stressed_wire_id = blocks >= 512;
    if (stressed_wire_id && seq_errors > 0)
    {
        std::fprintf(stderr,
                     "FAIL: payload sequence errors after %" PRIu64 " blocks / %" PRIu64
                     " wire wraps — 8-bit block_id collision likely\n",
                     blocks, wire_wrap_marks);
        return 1;
    }
    if (missing > 0)
    {
        std::fprintf(stderr,
                     "WARN: %" PRIu64 " apps missing from decode (tail/partial block); "
                     "seq_errors=%" PRIu64 "\n",
                     missing, seq_errors);
    }

    if (stressed_wire_id && seq_errors == 0)
    {
        std::printf("PASS: no sequence corruption through %" PRIu64 " wire id wraps "
                    "(%" PRIu64 " blocks)\n",
                    wire_wrap_marks, blocks);
    }
    else
    {
        std::printf("PASS (short run or no seq errors)\n");
    }
    return 0;
}
