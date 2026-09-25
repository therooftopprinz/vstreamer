#include "core/rs_block_erasure.hpp"
#include "core/stream_air_limits.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

using vstreamer::rs_block_erasure;

int main()
{
    rs_block_erasure enc;
    rs_block_erasure dec;
    if (!enc.init(10, 12, 20))
    {
        std::fprintf(stderr, "init failed\n");
        return 1;
    }

    std::vector<std::vector<uint8_t>> pending;
    for (int i = 0; i < 10; i++)
    {
        std::vector<uint8_t> pkt(200, static_cast<uint8_t>(i));
        pkt[0] = static_cast<uint8_t>(0x80);
        pkt[1] = static_cast<uint8_t>(i);
        enc.push_app(pkt.data(), pkt.size(), &pending);
    }
    if (pending.size() != 12U)
    {
        std::fprintf(stderr, "expected 12 shards got %zu\n", pending.size());
        return 1;
    }

    /* Drop two systematic shards; parity should recover. */
    std::vector<std::vector<uint8_t>> recovered;
    for (size_t i = 0; i < pending.size(); i++)
    {
        if (2 == i || 5 == i)
        {
            continue;
        }
        dec.push_air(pending[i].data(), pending[i].size(), &recovered);
    }
    if (recovered.size() != 10U)
    {
        std::fprintf(stderr, "expected 10 payloads got %zu fec_rec=%llu fec_lost=%llu\n",
                     recovered.size(), static_cast<unsigned long long>(dec.recovered()),
                     static_cast<unsigned long long>(dec.decode_fail()));
        return 1;
    }
    for (size_t i = 0; i < recovered.size(); i++)
    {
        if (recovered[i].size() != 200U || recovered[i][1] != static_cast<uint8_t>(i))
        {
            std::fprintf(stderr, "payload %zu mismatch\n", i);
            return 1;
        }
    }

    /* Blocks must be emitted in block_id order even when shards arrive OOO. */
    rs_block_erasure enc2;
    rs_block_erasure dec2;
    if (!enc2.init(2, 4, 20))
    {
        std::fprintf(stderr, "init2 failed\n");
        return 1;
    }
    std::vector<std::vector<uint8_t>> block0;
    std::vector<std::vector<uint8_t>> block1;
    for (int b = 0; b < 2; b++)
    {
        std::vector<std::vector<uint8_t>> pending;
        for (int i = 0; i < 2; i++)
        {
            std::vector<uint8_t> pkt(32, static_cast<uint8_t>(b * 10 + i));
            pkt[0] = static_cast<uint8_t>(0x80);
            pkt[3] = static_cast<uint8_t>(b * 2 + i);
            enc2.push_app(pkt.data(), pkt.size(), &pending);
        }
        if (b == 0)
        {
            block0 = pending;
        }
        else
        {
            block1 = pending;
        }
    }
    /* First shard of block0 arrives, then all of block1, then the rest of
     * block0: block1 must be held until block0 completes. */
    std::vector<std::vector<uint8_t>> ordered;
    std::vector<std::vector<uint8_t>> batch;
    dec2.push_air(block0[0].data(), block0[0].size(), &batch);
    ordered.insert(ordered.end(), batch.begin(), batch.end());
    for (size_t i = 0; i < block1.size(); i++)
    {
        dec2.push_air(block1[i].data(), block1[i].size(), &batch);
        ordered.insert(ordered.end(), batch.begin(), batch.end());
    }
    if (!ordered.empty())
    {
        std::fprintf(stderr, "ooo emit released block1 before block0 (%zu)\n", ordered.size());
        return 1;
    }
    for (size_t i = 1; i < block0.size(); i++)
    {
        dec2.push_air(block0[i].data(), block0[i].size(), &batch);
        ordered.insert(ordered.end(), batch.begin(), batch.end());
    }
    if (ordered.size() != 4U)
    {
        std::fprintf(stderr, "ooo emit expected 4 payloads got %zu\n", ordered.size());
        return 1;
    }
    for (int i = 0; i < 4; i++)
    {
        if (ordered[static_cast<size_t>(i)][3] != static_cast<uint8_t>(i))
        {
            std::fprintf(stderr, "ooo emit seq mismatch at %d\n", i);
            return 1;
        }
    }

    /* Many blocks, all shards: no decode failures or spurious app loss. */
    rs_block_erasure enc3;
    rs_block_erasure dec3;
    if (!enc3.init(10, 12, 20))
    {
        std::fprintf(stderr, "init3 failed\n");
        return 1;
    }
    std::vector<std::vector<uint8_t>> all_air;
    for (int b = 0; b < 50; b++)
    {
        std::vector<std::vector<uint8_t>> block_air;
        for (int i = 0; i < 10; i++)
        {
            std::vector<uint8_t> pkt(64, static_cast<uint8_t>(b));
            pkt[0] = static_cast<uint8_t>(0x80);
            enc3.push_app(pkt.data(), pkt.size(), &block_air);
        }
        all_air.insert(all_air.end(), block_air.begin(), block_air.end());
    }
    size_t payloads_out = 0;
    for (const auto& shard : all_air)
    {
        std::vector<std::vector<uint8_t>> batch;
        dec3.push_air(shard.data(), shard.size(), &batch);
        payloads_out += batch.size();
    }
    if (payloads_out != 500U)
    {
        std::fprintf(stderr, "lossless stream expected 500 payloads got %zu fail=%llu\n",
                     payloads_out, static_cast<unsigned long long>(dec3.decode_fail()));
        return 1;
    }
    if (dec3.decode_fail() != 0U)
    {
        std::fprintf(stderr, "lossless stream decode_fail=%llu\n",
                     static_cast<unsigned long long>(dec3.decode_fail()));
        return 1;
    }

    /* Helpers for the ordering/stall cases below. */
    auto make_block = [](rs_block_erasure& e, uint8_t tag) {
        std::vector<std::vector<uint8_t>> air;
        for (int i = 0; i < e.k(); i++)
        {
            std::vector<uint8_t> pkt(32, tag);
            pkt[0] = static_cast<uint8_t>(0x80);
            e.push_app(pkt.data(), pkt.size(), &air);
        }
        return air;
    };
    auto feed = [](rs_block_erasure& d, const std::vector<std::vector<uint8_t>>& air,
                   std::vector<std::vector<uint8_t>>& sink) {
        std::vector<std::vector<uint8_t>> b;
        for (const auto& s : air)
        {
            d.push_air(s.data(), s.size(), &b);
            sink.insert(sink.end(), b.begin(), b.end());
        }
    };
    auto block_id_of = [](const std::vector<std::vector<uint8_t>>& air) {
        return static_cast<uint16_t>(air[0][0]);
    };
    auto tick_after = [](rs_block_erasure& d, int ms, std::vector<std::vector<uint8_t>>& sink) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        std::vector<std::vector<uint8_t>> b;
        d.poll_rx(&b);
        sink.insert(sink.end(), b.begin(), b.end());
    };

    /* Receiver joining mid-stream must emit from the first block it sees. */
    {
        rs_block_erasure enc4;
        rs_block_erasure dec4;
        if (!enc4.init(2, 4, 20))
        {
            std::fprintf(stderr, "init4 failed\n");
            return 1;
        }
        (void)make_block(enc4, 1);
        (void)make_block(enc4, 2);
        const auto late_join = make_block(enc4, 3);
        std::vector<std::vector<uint8_t>> got;
        feed(dec4, late_join, got);
        if (got.size() != 2U)
        {
            std::fprintf(stderr, "mid-stream join expected 2 payloads got %zu\n", got.size());
            return 1;
        }
    }

    /* A block lost entirely on the wire must not stall later blocks forever;
     * a late copy of it is still delivered. */
    {
        rs_block_erasure enc5;
        rs_block_erasure dec5;
        if (!enc5.init(2, 4, 20))
        {
            std::fprintf(stderr, "init5 failed\n");
            return 1;
        }
        const auto a = make_block(enc5, 0xA);
        const auto b = make_block(enc5, 0xB);
        const auto c = make_block(enc5, 0xC);
        std::vector<std::vector<uint8_t>> got;
        feed(dec5, a, got);
        feed(dec5, c, got);
        if (got.size() != 2U)
        {
            std::fprintf(stderr, "hole: expected only block A (2) got %zu\n", got.size());
            return 1;
        }
        tick_after(dec5, 10, got);
        if (got.size() != 2U)
        {
            std::fprintf(stderr, "hole: released C too early (%zu)\n", got.size());
            return 1;
        }
        tick_after(dec5, dec5.emit_hold_ms() + 20, got);
        if (got.size() != 4U || got[2][1] != 0xC)
        {
            std::fprintf(stderr, "hole: expected C after emit_hold got %zu\n", got.size());
            return 1;
        }
        feed(dec5, b, got);
        if (got.size() != 6U || got[4][1] != 0xB)
        {
            std::fprintf(stderr, "hole: late B not delivered (%zu)\n", got.size());
            return 1;
        }
        const auto d = make_block(enc5, 0xD);
        feed(dec5, d, got);
        if (got.size() != 8U || got[6][1] != 0xD)
        {
            std::fprintf(stderr, "hole: stream did not resume after late B (%zu)\n", got.size());
            return 1;
        }
    }

    /* TX block_id continues across a runtime k/n re-init. */
    {
        rs_block_erasure enc6;
        if (!enc6.init(2, 4, 20))
        {
            std::fprintf(stderr, "init6 failed\n");
            return 1;
        }
        const uint16_t id0 = block_id_of(make_block(enc6, 1));
        if (!enc6.init(3, 5, 20))
        {
            std::fprintf(stderr, "reinit6 failed\n");
            return 1;
        }
        const uint16_t id1 = block_id_of(make_block(enc6, 2));
        if (id1 != static_cast<uint16_t>(id0 + 1))
        {
            std::fprintf(stderr, "block_id reset on reinit (%u -> %u)\n", id0, id1);
            return 1;
        }
    }

    /* Sender process restart (new random block_id base): receiver resyncs. */
    {
        rs_block_erasure enc7a;
        rs_block_erasure enc7b;
        rs_block_erasure dec7;
        if (!enc7a.init(2, 4, 20) || !enc7b.init(2, 4, 20))
        {
            std::fprintf(stderr, "init7 failed\n");
            return 1;
        }
        std::vector<std::vector<uint8_t>> got;
        feed(dec7, make_block(enc7a, 1), got);
        feed(dec7, make_block(enc7a, 2), got);
        feed(dec7, make_block(enc7b, 3), got);
        feed(dec7, make_block(enc7b, 4), got);
        tick_after(dec7, dec7.emit_hold_ms() + 20, got);
        if (got.size() != 8U)
        {
            std::fprintf(stderr, "restart: expected 8 payloads got %zu\n", got.size());
            return 1;
        }
        feed(dec7, make_block(enc7b, 5), got);
        if (got.size() != 10U || got[8][1] != 5)
        {
            std::fprintf(stderr, "restart: stream did not flow after resync (%zu)\n",
                         got.size());
            return 1;
        }
    }

    /* n=k: headered blocks, no parity shards. */
    {
        rs_block_erasure enc_nk;
        rs_block_erasure dec_nk;
        if (!enc_nk.init(4, 4, 20))
        {
            std::fprintf(stderr, "init n=k failed\n");
            return 1;
        }
        std::vector<std::vector<uint8_t>> block;
        for (int i = 0; i < 4; i++)
        {
            std::vector<uint8_t> pkt(48, static_cast<uint8_t>(i));
            enc_nk.push_app(pkt.data(), pkt.size(), &block);
        }
        if (block.size() != 4U)
        {
            std::fprintf(stderr, "n=k: expected 4 shards got %zu\n", block.size());
            return 1;
        }
        for (const auto& shard : block)
        {
            if (shard.size() < rs_block_erasure::k_header_len ||
                (shard[2] & 0xF) != 4 || ((shard[2] >> 4) & 0xF) != 4 || shard[3] != 4 ||
                (shard[1] & rs_block_erasure::k_wire_index_mask) >= 4 ||
                0 != (shard[1] & rs_block_erasure::k_wire_index_reserved) ||
                0 != (shard[3] & rs_block_erasure::k_wire_sdu_n_reserved))
            {
                std::fprintf(stderr, "n=k: bad shard header\n");
                return 1;
            }
        }
        std::vector<std::vector<uint8_t>> out;
        for (const auto& shard : block)
        {
            dec_nk.push_air(shard.data(), shard.size(), &out);
        }
        if (out.size() != 4U)
        {
            std::fprintf(stderr, "n=k: expected 4 payloads got %zu\n", out.size());
            return 1;
        }
    }

    /* Post-FEC gap uses wire N (SDUs in block), not RS k. */
    {
        rs_block_erasure enc_gap;
        rs_block_erasure dec_gap;
        if (!enc_gap.init(4, 4, 20))
        {
            std::fprintf(stderr, "init gap n=k failed\n");
            return 1;
        }
        std::vector<std::vector<uint8_t>> block;
        for (int i = 0; i < 4; i++)
        {
            std::vector<uint8_t> pkt(16, static_cast<uint8_t>(0x40 + i));
            enc_gap.push_app(pkt.data(), pkt.size(), &block);
        }
        std::vector<std::vector<uint8_t>> out;
        for (size_t i = 1; i < block.size(); i++)
        {
            dec_gap.push_air(block[i].data(), block[i].size(), &out);
        }
        tick_after(dec_gap, dec_gap.rx_hold_ms() + 30, out);
        const uint64_t lost = dec_gap.take_fail_lost_app_pkts();
        if (out.size() != 3U || lost != 1U)
        {
            std::fprintf(stderr,
                         "fec gap N: expected 3 payloads lost=1 got %zu lost=%llu\n",
                         out.size(), static_cast<unsigned long long>(lost));
            return 1;
        }
    }

    {
        rs_block_erasure enc_part;
        if (!enc_part.init(10, 12, 20))
        {
            std::fprintf(stderr, "init partial gap failed\n");
            return 1;
        }
        for (int i = 0; i < 3; i++)
        {
            std::vector<uint8_t> pkt(24, static_cast<uint8_t>(i));
            std::vector<std::vector<uint8_t>> tmp;
            enc_part.push_app(pkt.data(), pkt.size(), &tmp);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        std::vector<std::vector<uint8_t>> block;
        enc_part.on_tick(&block);
        if (block.empty() || block[0][3] != 3)
        {
            std::fprintf(stderr, "partial flush: expected header N=3 got %u\n",
                         block.empty() ? 0U : static_cast<unsigned>(block[0][3]));
            return 1;
        }
        if (block.size() != 5U)
        {
            std::fprintf(stderr, "partial flush: expected 5 on-air shards (3+parity) got %zu\n",
                         block.size());
            return 1;
        }
        rs_block_erasure dec_part;
        /* Skip SDU 0 and omit parity: too few shards to RS-decode. */
        for (size_t i = 1; i <= 2; i++)
        {
            std::vector<std::vector<uint8_t>> step;
            dec_part.push_air(block[i].data(), block[i].size(), &step);
        }
        std::vector<std::vector<uint8_t>> out;
        tick_after(dec_part, dec_part.rx_hold_ms() + 30, out);
        const uint64_t lost = dec_part.take_fail_lost_app_pkts();
        if (lost != 1U)
        {
            std::fprintf(stderr, "partial N gap: expected lost=1 got %llu out=%zu\n",
                         static_cast<unsigned long long>(lost), out.size());
            return 1;
        }
    }

    /* 8-bit wire block_id wrap (255 -> 0): in-order emit must stay lossless. */
    {
        rs_block_erasure enc8;
        rs_block_erasure dec8;
        if (!enc8.init(2, 2, 20))
        {
            std::fprintf(stderr, "init8 wrap failed\n");
            return 1;
        }
        std::vector<std::vector<uint8_t>> got;
        for (int i = 0; i < 258; i++)
        {
            feed(dec8, make_block(enc8, static_cast<uint8_t>(i & 0x7F)), got);
        }
        tick_after(dec8, dec8.emit_hold_ms() + 30, got);
        if (got.size() != 516U || dec8.decode_fail() != 0)
        {
            std::fprintf(stderr,
                         "block_id wrap: expected 516 payloads decode_fail=0 got %zu fail=%llu\n",
                         got.size(), static_cast<unsigned long long>(dec8.decode_fail()));
            return 1;
        }
    }

    {
        uint16_t bid = 0;
        int idx = 0;
        int kk = 0;
        int nn = 0;
        int sn = 0;
        uint8_t flags = 0;
        uint8_t good[4] = {7, 0x82, 0xC4, 0x03};
        if (!rs_block_erasure::unpack_header(good, 4, &bid, &idx, &kk, &nn, &flags, &sn) ||
            bid != 7 || idx != 2 || kk != 4 || nn != 12 || sn != 3 ||
            0 == (flags & rs_block_erasure::k_flag_parity))
        {
            std::fprintf(stderr, "header unpack good sample failed\n");
            return 1;
        }
        uint8_t bad_idx[4] = {0, 0x32, 0x44, 0x04};
        if (rs_block_erasure::unpack_header(bad_idx, 4, &bid, &idx, &kk, &nn, &flags, &sn))
        {
            std::fprintf(stderr, "header unpack should reject reserved index bits\n");
            return 1;
        }
        uint8_t bad_sn[4] = {0, 0x02, 0x44, 0x14};
        if (rs_block_erasure::unpack_header(bad_sn, 4, &bid, &idx, &kk, &nn, &flags, &sn))
        {
            std::fprintf(stderr, "header unpack should reject reserved sdu_n bits\n");
            return 1;
        }
    }

    std::printf("rs_fec_test ok (max_orig=%zu)\n", rs_block_erasure::max_original());
    return 0;
}
