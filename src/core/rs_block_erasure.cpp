#include "core/rs_block_erasure.hpp"

#include "core/stream_air_limits.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cstring>
#include <random>

extern "C"
{
#include "erasure_code.h"
}

#ifdef VSTREAMER_ISAL_NEON
extern "C" void vstreamer_ec_encode_data(int len, int k, int rows,
                                         unsigned char* g_tbls,
                                         unsigned char** data,
                                         unsigned char** coding);
#define VSTREAMER_EC_ENCODE vstreamer_ec_encode_data
#else
#define VSTREAMER_EC_ENCODE ec_encode_data_base
#endif

namespace
{
constexpr size_t k_min_shard = 16;  // ISA-L NEON kernels need >= 16 bytes.

void store_be16(uint8_t* p, uint16_t v)
{
    const uint16_t n = htons(v);
    memcpy(p, &n, sizeof(n));
}

uint16_t load_be16(const uint8_t* p)
{
    uint16_t n;
    memcpy(&n, p, sizeof(n));
    return ntohs(n);
}

size_t align_shard(size_t row)
{
    return row < k_min_shard ? k_min_shard : row;
}

void add_virtual_empty_systematic(
    int sdu_n, int k, std::unordered_map<int, std::vector<uint8_t>>& frags)
{
    for (int i = sdu_n; i < k; i++)
    {
        if (frags.find(i) != frags.end())
        {
            continue;
        }
        frags.emplace(i, std::vector<uint8_t>(vstreamer::rs_block_erasure::k_len_prefix, 0));
    }
}
}  // namespace

size_t vstreamer::rs_block_erasure::max_original()
{
    return k_stream_payload_max - k_header_len - k_len_prefix;
}

const char* vstreamer::rs_block_erasure::impl_name() const
{
#ifdef VSTREAMER_ISAL_NEON
    return "isa-l neon";
#else
    return "isa-l base";
#endif
}

vstreamer::rs_block_erasure::rs_block_erasure()
{
    // Random TX start id: a restarted sender must not replay ids that the
    // receiver's duplicate filter / in-order emit queue still remembers.
    std::random_device rd;
    block_id = static_cast<uint16_t>(rd());
}

bool vstreamer::rs_block_erasure::init(int k, int n, int timeout_ms)
{
    enabled_ = false;
    if (k < k_header_k_n_min || n < k || n > k_header_k_n_max ||
        k > k_header_k_n_max || timeout_ms < 0)
    {
        return false;
    }
    k_ = k;
    n_ = n;
    p = n - k;
    this->timeout_ms = timeout_ms;
    encode_matrix.assign(static_cast<size_t>(n_) * static_cast<size_t>(k_), 0);
    g_tbls.assign(static_cast<size_t>(k_) * static_cast<size_t>(p) * 32, 0);
    gf_gen_cauchy1_matrix(encode_matrix.data(), n_, k_);
    ec_init_tables_base(k_, p, encode_matrix.data() + k_ * k_,
                        g_tbls.data());
    pending.clear();
    deadline_set = false;
    rx_blocks.clear();
    done_order.clear();
    done.clear();
    emit_pending.clear();
    emit_skipped.clear();
    emit_base_set = false;
    emit_next = 0;
    emit_waiting = false;
    recovered_ = 0;
    recovered_seen_ = 0;
    blocks_ = 0;
    decode_fail_ = 0;
    decode_fail_seen_ = 0;
    fail_missing_shards_ = 0;
    fail_missing_shards_seen_ = 0;
    fail_lost_app_pkts_ = 0;
    fail_lost_app_pkts_seen_ = 0;
    oversized_ = 0;
    enabled_ = true;
    return true;
}

void vstreamer::rs_block_erasure::disable()
{
    enabled_ = false;
    pending.clear();
    deadline_set = false;
    k_ = 0;
    n_ = 0;
    p = 0;
    encode_matrix.clear();
    g_tbls.clear();
    rx_blocks.clear();
    done_order.clear();
    done.clear();
    emit_pending.clear();
    emit_skipped.clear();
    emit_base_set = false;
    emit_next = 0;
    emit_waiting = false;
}

void vstreamer::rs_block_erasure::note_emit_base(uint16_t block_id)
{
    if (!emit_base_set)
    {
        emit_base_set = true;
        emit_next = wire_block_id(block_id);
        emit_waiting = false;
    }
}

int vstreamer::rs_block_erasure::emit_distance(uint16_t block_id) const
{
    return static_cast<int16_t>(static_cast<uint16_t>(wire_block_id(block_id) - emit_next));
}

void vstreamer::rs_block_erasure::prune_emit_behind()
{
    for (auto it = emit_skipped.begin(); it != emit_skipped.end();)
    {
        if (emit_distance(*it) < 0)
        {
            it = emit_skipped.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void vstreamer::rs_block_erasure::advance_emit_past_hole()
{
    // Jump emit_next to the nearest queued block ahead of it.
    bool     found = false;
    uint16_t best = 0;
    int      best_d = 0;
    for (const auto& kv : emit_pending)
    {
        const int d = emit_distance(kv.first);
        if (!found || d < best_d)
        {
            found = true;
            best = kv.first;
            best_d = d;
        }
    }
    if (found)
    {
        emit_next = wire_block_id(best);
        prune_emit_behind();
    }
}

void vstreamer::rs_block_erasure::skip_emit_block(uint16_t block_id)
{
    note_emit_base(block_id);
    emit_pending.erase(block_id);
    if (emit_distance(block_id) >= 0)
    {
        emit_skipped.insert(block_id);
    }
}

void vstreamer::rs_block_erasure::drain_emit_queue(
    std::vector<std::vector<uint8_t>>* out)
{
    if (out == nullptr || !emit_base_set)
    {
        return;
    }
    const auto t = now();
    for (;;)
    {
        auto it = emit_pending.find(emit_next);
        if (it != emit_pending.end())
        {
            for (auto& row : it->second)
            {
                out->push_back(std::move(row));
            }
            emit_pending.erase(it);
            emit_skipped.erase(emit_next);
            emit_next = wire_block_id(static_cast<uint16_t>(emit_next + 1));
            emit_waiting = false;
            continue;
        }
        if (emit_skipped.count(emit_next) != 0)
        {
            emit_skipped.erase(emit_next);
            emit_next = wire_block_id(static_cast<uint16_t>(emit_next + 1));
            emit_waiting = false;
            continue;
        }
        if (emit_pending.empty())
        {
            emit_waiting = false;
            break;
        }
        // Head-of-line hole: emit_next was never seen, is still assembling,
        // or belongs to a peer that restarted. Wait a bounded reorder window,
        // then skip past it so later blocks are not held (or leaked) forever.
        if (!emit_waiting)
        {
            emit_waiting = true;
            emit_wait_since = t;
        }
        const bool timed_out =
            t - emit_wait_since > std::chrono::milliseconds(emit_hold_ms());
        if (!timed_out && emit_pending.size() <= k_block_max)
        {
            break;
        }
        advance_emit_past_hole();
        emit_waiting = false;
    }
}

void vstreamer::rs_block_erasure::queue_decoded_block(
    uint16_t block_id, std::vector<std::vector<uint8_t>> payloads,
    std::vector<std::vector<uint8_t>>* out)
{
    note_emit_base(block_id);
    if (payloads.empty())
    {
        skip_emit_block(block_id);
        drain_emit_queue(out);
        return;
    }
    const int d = emit_distance(block_id);
    if (d < -static_cast<int>(k_wire_block_id_mod / 2))
    {
        // Far behind emit_next: the peer restarted its id counter. Release
        // whatever is queued from the old stream and resync on this block.
        std::vector<std::pair<int, uint16_t>> order;
        for (const auto& kv : emit_pending)
        {
            order.emplace_back(emit_distance(kv.first), kv.first);
        }
        std::sort(order.begin(), order.end());
        for (const auto& e : order)
        {
            for (auto& row : emit_pending[e.second])
            {
                if (out != nullptr)
                {
                    out->push_back(std::move(row));
                }
            }
        }
        emit_pending.clear();
        emit_skipped.clear();
        emit_waiting = false;
        emit_next = wire_block_id(block_id);
    }
    else if (d < 0)
    {
        // Late block (queue already advanced past it): deliver now rather
        // than drop k app packets.
        if (out != nullptr)
        {
            for (auto& row : payloads)
            {
                out->push_back(std::move(row));
            }
        }
        return;
    }
    emit_pending[block_id] = std::move(payloads);
    drain_emit_queue(out);
}

bool vstreamer::rs_block_erasure::pack_header(uint8_t* out, uint16_t block_id, int index,
                                 int k, int n, uint8_t flags, int sdu_n)
{
    if (out == nullptr || index < 0 || index >= n || index > k_wire_index_mask ||
        k < k_header_k_n_min || k > k_header_k_n_max || n < k ||
        n > k_header_k_n_max || sdu_n < 0 || sdu_n > k)
    {
        return false;
    }
    out[0] = static_cast<uint8_t>(block_id & 0xFF);
    uint8_t index_flag = static_cast<uint8_t>(index & k_wire_index_mask);
    if (0 != (flags & k_flag_parity))
    {
        index_flag = static_cast<uint8_t>(index_flag | k_flag_parity);
    }
    out[1] = index_flag;
    out[2] = static_cast<uint8_t>(((n & 0xF) << 4) | (k & 0xF));
    out[3] = static_cast<uint8_t>(sdu_n & k_wire_index_mask);
    return true;
}

bool vstreamer::rs_block_erasure::unpack_header(const uint8_t* data, size_t len,
                                   uint16_t* block_id, int* index, int* k,
                                   int* n, uint8_t* flags, int* sdu_n)
{
    if (data == nullptr || len < k_header_len || block_id == nullptr || index == nullptr ||
        k == nullptr || n == nullptr || flags == nullptr || sdu_n == nullptr)
    {
        return false;
    }
    const uint8_t kn = data[2];
    const int kk = static_cast<int>(kn & 0xF);
    const int nn = static_cast<int>((kn >> 4) & 0xF);
    const uint8_t index_flag = data[1];
    if (0 != (index_flag & k_wire_index_reserved) ||
        0 != (data[3] & k_wire_sdu_n_reserved))
    {
        return false;
    }
    const int idx = static_cast<int>(index_flag & k_wire_index_mask);
    const int sn = static_cast<int>(data[3] & k_wire_index_mask);
    if (kk < k_header_k_n_min || kk > k_header_k_n_max || nn < kk ||
        nn > k_header_k_n_max || idx >= nn || sn < 0 || sn > kk)
    {
        return false;
    }
    *block_id = data[0];
    *index = idx;
    *k = kk;
    *n = nn;
    *sdu_n = sn;
    *flags = (0 != (index_flag & k_flag_parity)) ? k_flag_parity : 0;
    return true;
}

bool vstreamer::rs_block_erasure::encode_block(
    const std::vector<std::vector<uint8_t>>& packets, uint16_t block_id,
    std::vector<std::vector<uint8_t>>* out) const
{
    if (!enabled_ || out == nullptr || packets.size() > static_cast<size_t>(k_))
    {
        return false;
    }
    std::vector<std::vector<uint8_t>> data_shards(static_cast<size_t>(k_));
    size_t max_row = 0;
    for (int i = 0; i < k_; i++)
    {
        const std::vector<uint8_t>* pkt = i < static_cast<int>(packets.size())
                                              ? &packets[static_cast<size_t>(i)]
                                              : nullptr;
        const size_t plen = pkt != nullptr ? pkt->size() : 0;
        if (plen > max_original())
        {
            return false;
        }
        data_shards[static_cast<size_t>(i)].resize(k_len_prefix + plen);
        store_be16(data_shards[static_cast<size_t>(i)].data(),
                   static_cast<uint16_t>(plen));
        if (plen > 0)
        {
            memcpy(data_shards[static_cast<size_t>(i)].data() + k_len_prefix,
                   pkt->data(), plen);
        }
        max_row = std::max(max_row, data_shards[static_cast<size_t>(i)].size());
    }
    const size_t shard_len = align_shard(max_row);
    if (k_header_len + shard_len > k_stream_payload_max)
    {
        return false;
    }
    for (int i = 0; i < k_; i++)
    {
        data_shards[static_cast<size_t>(i)].resize(shard_len, 0);
    }
    std::vector<std::vector<uint8_t>> parity(static_cast<size_t>(p));
    std::vector<unsigned char*> data_ptrs(static_cast<size_t>(k_));
    std::vector<unsigned char*> coding_ptrs(static_cast<size_t>(p));
    for (int i = 0; i < k_; i++)
    {
        data_ptrs[static_cast<size_t>(i)] =
            data_shards[static_cast<size_t>(i)].data();
    }
    for (int i = 0; i < p; i++)
    {
        parity[static_cast<size_t>(i)].assign(shard_len, 0);
        coding_ptrs[static_cast<size_t>(i)] =
            parity[static_cast<size_t>(i)].data();
    }
    if (p > 0)
    {
        VSTREAMER_EC_ENCODE(static_cast<int>(shard_len), k_, p,
                          const_cast<unsigned char*>(g_tbls.data()),
                          data_ptrs.data(), coding_ptrs.data());
    }

    out->clear();
    const int sdu_n = static_cast<int>(packets.size());
    for (int i = 0; i < n_; i++)
    {
        if (i < k_ && i >= sdu_n)
        {
            continue;
        }
        const uint8_t flags = i >= k_ ? k_flag_parity : 0;
        size_t body_len = shard_len;
        const uint8_t* body = nullptr;
        if (i < k_)
        {
            const size_t plen = packets[static_cast<size_t>(i)].size();
            body_len = k_len_prefix + plen;
            body = data_shards[static_cast<size_t>(i)].data();
        }
        else
        {
            body = parity[static_cast<size_t>(i - k_)].data();
        }
        std::vector<uint8_t> pkt(k_header_len + body_len);
        pack_header(pkt.data(), block_id, i, k_, n_, flags, sdu_n);
        memcpy(pkt.data() + k_header_len, body, body_len);
        out->push_back(std::move(pkt));
    }
    return true;
}

int vstreamer::rs_block_erasure::gen_decode_matrix(int k, int n,
                                        const uint8_t* encode_matrix,
                                        const uint8_t* err_list, int nerrs,
                                        uint8_t* decode_matrix,
                                        uint8_t* decode_index)
{
    if (encode_matrix == nullptr || err_list == nullptr ||
        decode_matrix == nullptr || decode_index == nullptr || k < 1 ||
        n <= k)
    {
        return -1;
    }
    std::vector<uint8_t> in_err(static_cast<size_t>(n), 0);
    for (int i = 0; i < nerrs; i++)
    {
        if (err_list[i] >= n)
        {
            return -1;
        }
        in_err[err_list[i]] = 1;
    }
    std::vector<uint8_t> b(static_cast<size_t>(k) * static_cast<size_t>(k));
    std::vector<uint8_t> invert(static_cast<size_t>(k) *
                                static_cast<size_t>(k));
    int r = 0;
    for (int i = 0; i < k; i++, r++)
    {
        while (r < n && in_err[static_cast<size_t>(r)])
        {
            r++;
        }
        if (r >= n)
        {
            return -1;
        }
        memcpy(b.data() + static_cast<size_t>(k) * static_cast<size_t>(i),
               encode_matrix + static_cast<size_t>(k) * static_cast<size_t>(r),
               static_cast<size_t>(k));
        decode_index[i] = static_cast<uint8_t>(r);
    }
    if (gf_invert_matrix(b.data(), invert.data(), k) < 0)
    {
        return -1;
    }
    for (int e = 0; e < nerrs; e++)
    {
        const int idx = err_list[e];
        if (idx < k)
        {
            memcpy(decode_matrix +
                       static_cast<size_t>(k) * static_cast<size_t>(e),
                   invert.data() +
                       static_cast<size_t>(k) * static_cast<size_t>(idx),
                   static_cast<size_t>(k));
            continue;
        }
        for (int i = 0; i < k; i++)
        {
            uint8_t s = 0;
            for (int j = 0; j < k; j++)
            {
                s ^= gf_mul(
                    invert[static_cast<size_t>(j) * static_cast<size_t>(k) +
                           static_cast<size_t>(i)],
                    encode_matrix[static_cast<size_t>(k) *
                                       static_cast<size_t>(idx) +
                                   static_cast<size_t>(j)]);
            }
            decode_matrix[static_cast<size_t>(k) * static_cast<size_t>(e) +
                          static_cast<size_t>(i)] = s;
        }
    }
    return 0;
}

bool vstreamer::rs_block_erasure::decode_block(
    const std::unordered_map<int, std::vector<uint8_t>>& frags,
    std::vector<std::vector<uint8_t>>* payloads, int* recovered) const
{
    if (!enabled_)
    {
        return false;
    }
    return decode_block(k_, n_, frags, payloads, recovered);
}

bool vstreamer::rs_block_erasure::decode_block(
    int k, int n, const std::unordered_map<int, std::vector<uint8_t>>& frags,
    std::vector<std::vector<uint8_t>>* payloads, int* recovered) const
{
    if (payloads == nullptr || k < 1 || n < k || n > static_cast<int>(k_max_n) ||
        frags.size() < static_cast<size_t>(k))
    {
        return false;
    }
    const int parity = n - k;
    std::vector<uint8_t> encode_matrix(static_cast<size_t>(n) *
                                       static_cast<size_t>(k));
    gf_gen_cauchy1_matrix(encode_matrix.data(), n, k);

    size_t shard_len = 0;
    for (const auto& kv : frags)
    {
        if (kv.first < 0 || kv.first >= n)
        {
            return false;
        }
        shard_len = std::max(shard_len, kv.second.size());
    }
    if (shard_len < k_len_prefix)
    {
        return false;
    }
    // Systematic rows may be native-size; pad to the longest (usually parity)
    // so ISA-L columns match encode.
    shard_len = align_shard(shard_len);
    std::vector<std::vector<uint8_t>> padded(static_cast<size_t>(n));
    std::vector<uint8_t> have(static_cast<size_t>(n), 0);
    for (const auto& kv : frags)
    {
        auto& row = padded[static_cast<size_t>(kv.first)];
        row = kv.second;
        if (row.size() < shard_len)
        {
            row.resize(shard_len, 0);
        }
        have[static_cast<size_t>(kv.first)] = 1;
    }

    std::vector<std::vector<uint8_t>> data_copy(static_cast<size_t>(k));
    bool have_all_data = true;
    for (int i = 0; i < k; i++)
    {
        if (!have[static_cast<size_t>(i)])
        {
            have_all_data = false;
            continue;
        }
        data_copy[static_cast<size_t>(i)] = padded[static_cast<size_t>(i)];
    }

    int rec = 0;
    if (!have_all_data)
    {
        uint8_t err_list[k_max_n];
        int nerrs = 0;
        for (int i = 0; i < n; i++)
        {
            if (!have[static_cast<size_t>(i)])
            {
                err_list[nerrs++] = static_cast<uint8_t>(i);
            }
        }
        if (nerrs > parity)
        {
            return false;
        }
        std::vector<uint8_t> decode_matrix(static_cast<size_t>(nerrs) *
                                           static_cast<size_t>(k));
        uint8_t decode_index[k_max_n];
        if (gen_decode_matrix(k, n, encode_matrix.data(), err_list, nerrs,
                              decode_matrix.data(), decode_index) != 0)
        {
            return false;
        }
        std::vector<uint8_t> decode_tbls(static_cast<size_t>(k) *
                                         static_cast<size_t>(nerrs) * 32);
        std::vector<unsigned char*> src_ptrs(static_cast<size_t>(k));
        std::vector<std::vector<uint8_t>> recover(static_cast<size_t>(nerrs));
        std::vector<unsigned char*> rec_ptrs(static_cast<size_t>(nerrs));
        for (int i = 0; i < k; i++)
        {
            const int idx = decode_index[i];
            if (!have[static_cast<size_t>(idx)])
            {
                return false;
            }
            src_ptrs[static_cast<size_t>(i)] =
                padded[static_cast<size_t>(idx)].data();
        }
        for (int i = 0; i < nerrs; i++)
        {
            recover[static_cast<size_t>(i)].assign(shard_len, 0);
            rec_ptrs[static_cast<size_t>(i)] =
                recover[static_cast<size_t>(i)].data();
        }
        ec_init_tables_base(k, nerrs, decode_matrix.data(),
                            decode_tbls.data());
        VSTREAMER_EC_ENCODE(static_cast<int>(shard_len), k, nerrs,
                          decode_tbls.data(), src_ptrs.data(), rec_ptrs.data());
        for (int i = 0; i < nerrs; i++)
        {
            const int idx = err_list[i];
            if (idx < k)
            {
                data_copy[static_cast<size_t>(idx)] =
                    std::move(recover[static_cast<size_t>(i)]);
                rec++;
            }
        }
    }

    payloads->clear();
    for (int i = 0; i < k; i++)
    {
        const auto& row = data_copy[static_cast<size_t>(i)];
        if (row.size() < k_len_prefix)
        {
            return false;
        }
        const uint16_t orig_len = load_be16(row.data());
        if (orig_len == 0)
        {
            continue;
        }
        if (k_len_prefix + orig_len > row.size())
        {
            return false;
        }
        payloads->emplace_back(row.data() + k_len_prefix,
                               row.data() + k_len_prefix + orig_len);
    }
    if (recovered != nullptr)
    {
        *recovered = rec;
    }
    return true;
}

void vstreamer::rs_block_erasure::push_app(const uint8_t* data, size_t len,
                              std::vector<std::vector<uint8_t>>* out)
{
    if (out != nullptr)
    {
        out->clear();
    }
    if (!enabled_ || out == nullptr || data == nullptr)
    {
        return;
    }
    if (len > max_original())
    {
        oversized_++;
        return;
    }
    pending.emplace_back(data, data + len);
    if (!deadline_set)
    {
        deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
        deadline_set = true;
    }
    if (static_cast<int>(pending.size()) >= k_)
    {
        flush(out);
    }
}

void vstreamer::rs_block_erasure::flush(std::vector<std::vector<uint8_t>>* out)
{
    if (out != nullptr)
    {
        out->clear();
    }
    if (!enabled_ || out == nullptr || pending.empty())
    {
        return;
    }
    if (!encode_block(pending, block_id, out))
    {
        oversized_++;
        out->clear();
        pending.clear();
        deadline_set = false;
        return;
    }
    block_id = static_cast<uint16_t>(block_id + 1);
    pending.clear();
    deadline_set = false;
    blocks_++;
}

int vstreamer::rs_block_erasure::rx_hold_ms() const
{
    return std::max(timeout_ms * 10, 250);
}

int count_app_packets_in_frags(
    const std::unordered_map<int, std::vector<uint8_t>>& frags, int k)
{
    int n = 0;
    for (int i = 0; i < k; i++)
    {
        const auto it = frags.find(i);
        if (it == frags.end())
        {
            continue;
        }
        const auto& row = it->second;
        if (row.size() < vstreamer::rs_block_erasure::k_len_prefix)
        {
            continue;
        }
        const uint16_t orig_len = load_be16(row.data());
        if (orig_len > 0)
        {
            n++;
        }
    }
    return n;
}

void vstreamer::rs_block_erasure::note_rx_block_loss(const rx_block_s& block)
{
    (void)block;
}

void vstreamer::rs_block_erasure::note_rx_block_output_shortfall(int expected, int available)
{
    if (expected > available)
    {
        fail_lost_app_pkts_ += static_cast<uint64_t>(expected - available);
    }
}

int vstreamer::rs_block_erasure::expected_sdus(const rx_block_s& block)
{
    if (block.sdu_n > 0)
    {
        return block.sdu_n;
    }
    return block.k;
}

void vstreamer::rs_block_erasure::finish_block_with_available(
    const rx_block_s& block, uint16_t block_id, std::vector<std::vector<uint8_t>>* out)
{
    int available = 0;
    for (int i = 0; i < block.k; i++)
    {
        const auto it = block.frags.find(i);
        if (it == block.frags.end())
        {
            continue;
        }
        const auto& row = it->second;
        if (row.size() < k_len_prefix)
        {
            continue;
        }
        const uint16_t orig_len = load_be16(row.data());
        if (orig_len == 0)
        {
            continue;
        }
        if (k_len_prefix + orig_len > row.size())
        {
            continue;
        }
        ++available;
        if (out != nullptr)
        {
            out->emplace_back(row.data() + k_len_prefix, row.data() + k_len_prefix + orig_len);
        }
    }
    note_rx_block_output_shortfall(expected_sdus(block), available);
    skip_emit_block(block_id);
}

int vstreamer::rs_block_erasure::done_hold_ms() const
{
    return std::max(timeout_ms * 50, rx_hold_ms());
}

int vstreamer::rs_block_erasure::emit_hold_ms() const
{
    // Shards of block N are all sent before block N+1 starts, so a block
    // that completes after a later one is only a few datagrams reordered;
    // a short window suffices and keeps head-of-line delay bounded.
    return std::max(timeout_ms * 3, 60);
}

void vstreamer::rs_block_erasure::poll_rx(std::vector<std::vector<uint8_t>>* out)
{
    if (out != nullptr)
    {
        out->clear();
    }
    expire_rx(out);
    drain_emit_queue(out);
}

std::chrono::steady_clock::time_point vstreamer::rs_block_erasure::now() const
{
    return std::chrono::steady_clock::now();
}

void vstreamer::rs_block_erasure::on_tick(std::vector<std::vector<uint8_t>>* out)
{
    if (out != nullptr)
    {
        out->clear();
    }
    // Always expire incomplete RX blocks (RX decode needs no encode init).
    expire_rx(out);
    if (!enabled_ || !deadline_set || timeout_ms <= 0)
    {
        return;
    }
    if (std::chrono::steady_clock::now() < deadline)
    {
        return;
    }
    flush(out);
}

void vstreamer::rs_block_erasure::expire_rx(std::vector<std::vector<uint8_t>>* out)
{
    expire_done();
    if (rx_blocks.empty())
    {
        return;
    }
    const auto t = now();
    const auto hold = std::chrono::milliseconds(rx_hold_ms());
    for (auto it = rx_blocks.begin(); it != rx_blocks.end();)
    {
        if (t - it->second.last_seen > hold)
        {
            decode_fail_++;
            note_rx_block_loss(it->second);
            const uint16_t expired_id = it->first;
            const rx_block_s block_snap = it->second;
            it = rx_blocks.erase(it);
            finish_block_with_available(block_snap, expired_id, out);
        }
        else
        {
            ++it;
        }
    }
}

void vstreamer::rs_block_erasure::expire_done()
{
    const auto t = now();
    const auto hold = std::chrono::milliseconds(done_hold_ms());
    while (!done_order.empty())
    {
        const uint16_t id = done_order.front();
        auto it = done.find(id);
        if (it == done.end())
        {
            done_order.pop_front();
            continue;
        }
        if (t - it->second <= hold)
        {
            break;
        }
        done.erase(it);
        done_order.pop_front();
    }
}

void vstreamer::rs_block_erasure::mark_done(uint16_t block_id)
{
    const auto t = now();
    auto inserted = done.emplace(block_id, t);
    if (inserted.second)
    {
        done_order.push_back(block_id);
    }
    else
    {
        inserted.first->second = t;
    }
    while (done_order.size() > k_done_max)
    {
        done.erase(done_order.front());
        done_order.pop_front();
    }
}

void vstreamer::rs_block_erasure::push_air(const uint8_t* data, size_t len,
                              std::vector<std::vector<uint8_t>>* out)
{
    if (out != nullptr)
    {
        out->clear();
    }
    if (out == nullptr || data == nullptr || len == 0)
    {
        return;
    }
    // RX is always FEC-aware: k/n come from the shard header. Encode still
    // requires init().
    if (len < k_header_len)
    {
        decode_fail_++;
        return;
    }
    struct push_air_finish
    {
        rs_block_erasure*                     self;
        std::vector<std::vector<uint8_t>>* out;
        ~push_air_finish()
        {
            self->expire_rx(out);
            self->drain_emit_queue(out);
        }
    } finish {this, out};
    uint16_t block_id = 0;
    int index = 0;
    int k = 0;
    int n = 0;
    int sdu_n = 0;
    uint8_t flags = 0;
    if (!unpack_header(data, len, &block_id, &index, &k, &n, &flags, &sdu_n))
    {
        decode_fail_++;
        return;
    }
    // In-order emission starts from the first block id seen on the wire so
    // a receiver joining mid-stream does not wait for id 0.
    note_emit_base(block_id);
    if (done.find(block_id) != done.end())
    {
        return;
    }
    rx_block_s* buf = nullptr;
    auto it = rx_blocks.find(block_id);
    if (it == rx_blocks.end())
    {
        while (rx_blocks.size() >= k_block_max)
        {
            auto oldest = rx_blocks.begin();
            for (auto j = rx_blocks.begin(); j != rx_blocks.end(); ++j)
            {
                if (j->second.last_seen < oldest->second.last_seen)
                {
                    oldest = j;
                }
            }
            const uint16_t evict_id = oldest->first;
            const rx_block_s block_snap = oldest->second;
            decode_fail_++;
            note_rx_block_loss(block_snap);
            rx_blocks.erase(oldest);
            finish_block_with_available(block_snap, evict_id, out);
        }
        rx_block_s nb;
        nb.k = k;
        nb.n = n;
        nb.sdu_n = sdu_n;
        const auto t_now = now();
        nb.first_seen = t_now;
        nb.last_seen = t_now;
        it = rx_blocks.emplace(block_id, std::move(nb)).first;
    }
    else if (it->second.k != k || it->second.n != n || it->second.sdu_n != sdu_n)
    {
        decode_fail_++;
        const rx_block_s block_snap = it->second;
        rx_blocks.erase(it);
        finish_block_with_available(block_snap, block_id, out);
        return;
    }
    buf = &it->second;
    if (buf->frags.count(index) != 0)
    {
        return;
    }
    buf->frags.emplace(index,
                       std::vector<uint8_t>(data + k_header_len, data + len));
    buf->last_seen = now();
    if (static_cast<int>(buf->frags.size()) < sdu_n)
    {
        return;
    }
    std::unordered_map<int, std::vector<uint8_t>> frags_decode = buf->frags;
    add_virtual_empty_systematic(sdu_n, k, frags_decode);
    if (frags_decode.size() < static_cast<size_t>(k))
    {
        return;
    }
    int rec = 0;
    std::vector<std::vector<uint8_t>> decoded;
    const rx_block_s block_snap = *buf;
    const bool ok = decode_block(k, n, frags_decode, &decoded, &rec);
    rx_blocks.erase(block_id);
    mark_done(block_id);
    if (!ok)
    {
        decode_fail_++;
        finish_block_with_available(block_snap, block_id, out);
        return;
    }
    recovered_ += static_cast<uint64_t>(rec);
    blocks_++;
    note_rx_block_output_shortfall(expected_sdus(block_snap),
                                   static_cast<int>(decoded.size()));
    queue_decoded_block(block_id, std::move(decoded), out);
}

uint64_t vstreamer::rs_block_erasure::take_recovered()
{
    const uint64_t n = recovered_ - recovered_seen_;
    recovered_seen_ = recovered_;
    return n;
}

uint64_t vstreamer::rs_block_erasure::take_decode_fail()
{
    const uint64_t n = decode_fail_ - decode_fail_seen_;
    decode_fail_seen_ = decode_fail_;
    return n;
}

uint64_t vstreamer::rs_block_erasure::take_fail_missing_shards()
{
    const uint64_t n = fail_missing_shards_ - fail_missing_shards_seen_;
    fail_missing_shards_seen_ = fail_missing_shards_;
    return n;
}

uint64_t vstreamer::rs_block_erasure::take_fail_lost_app_pkts()
{
    const uint64_t n = fail_lost_app_pkts_ - fail_lost_app_pkts_seen_;
    fail_lost_app_pkts_seen_ = fail_lost_app_pkts_;
    return n;
}
