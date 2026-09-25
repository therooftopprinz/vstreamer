#ifndef VSTREAMER_CORE_RS_BLOCK_ERASURE_HPP
#define VSTREAMER_CORE_RS_BLOCK_ERASURE_HPP

#include <chrono>
#include <deque>
#include <stddef.h>
#include <stdint.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Packet-block Reed-Solomon erasure FEC (systematic Cauchy MDS via ISA-L).
// k app datagrams become n on-air shards. Wire header is 4 bytes (see pack_header).
// n == k: no parity (non-FEC redundancy); same block framing and header on each shard.
// Wire (4 bytes, 32 bits):
//   [0] block_id (8)
//   [1] parity(1) | reserved(3) | shard_index(4)   — index < n <= 15
//   [2] n(4) | k(4)
//   [3] reserved(4) | sdu_n(4)                     — sdu_n <= k
// Spare for extensions: 8 bits if parity is derived (index >= k); we still
// set the parity bit on TX and require byte1 bits 6..4 and byte3 bits 7..4
// zero on RX (7 bits reserved today; the parity bit is the 8th logical spare).
namespace vstreamer
{

class rs_block_erasure
{
public:
    static constexpr size_t k_header_len = 4;
    static constexpr size_t k_len_prefix = 2;
    static constexpr uint8_t k_flag_parity = 0x80;          /* bit 7 of byte 1 */
    static constexpr uint8_t k_wire_index_mask = 0x0F;      /* byte 1 bits 3..0 */
    static constexpr uint8_t k_wire_index_reserved = 0x70; /* byte 1 bits 6..4 */
    static constexpr uint8_t k_wire_sdu_n_reserved = 0xF0;  /* byte 3 bits 7..4 */
    static constexpr int k_header_k_n_min = 1;
    static constexpr int k_header_k_n_max = 15; /* 4-bit k and n on wire */
    static constexpr int k_max_wire_n = k_header_k_n_max;
    static constexpr int k_default_timeout_ms = 20;
    static constexpr size_t k_max_n = 255;
    static constexpr size_t k_done_max = 128;
    static constexpr size_t k_block_max = 256;
    /* block_id on the 4-byte shard header is one byte; emit/dedupe use this ring. */
    static constexpr uint16_t k_wire_block_id_mod = 256;

    [[nodiscard]] static constexpr uint16_t wire_block_id(uint16_t id)
    {
        return static_cast<uint16_t>(id & (k_wire_block_id_mod - 1U));
    }

    // Incomplete RX block TTL, and finished-id TTL (duplicate-shard
    // suppression). done_hold is longer so a late shard of a completed block
    // is still dropped, but short enough that a peer restart which reuses
    // block_id from 0 is accepted after the old ids age out.
    int rx_hold_ms() const;
    int done_hold_ms() const;
    // Max time a decoded block waits in the in-order emit queue for an
    // earlier block_id before the queue advances past the hole.
    int emit_hold_ms() const;

    rs_block_erasure();

    // init() / disable() keep the TX block_id counter running so a runtime
    // k/n change does not replay ids the receiver already saw.
    bool init(int k, int n, int timeout_ms);
    // Stop TX encode; flush pending first via flush()/announce_down. RX decode
    // still works from shard headers.
    void disable();
    bool enabled() const
    {
        return enabled_;
    }
    int k() const
    {
        return k_;
    }
    int n() const
    {
        return n_;
    }
    const char* impl_name() const;

    // App datagram -> zero or more air shards (full block or nothing).
    void push_app(const uint8_t* data, size_t len,
                  std::vector<std::vector<uint8_t>>* out);
    void flush(std::vector<std::vector<uint8_t>>* out);
    // TX tick: timeout-flush a partial block. Also expires RX state.
    void on_tick(std::vector<std::vector<uint8_t>>* out);
    // RX tick: expire stale blocks and release payloads held in the
    // in-order emit queue whose head-of-line wait has elapsed. Call
    // periodically even when no datagram arrives.
    void poll_rx(std::vector<std::vector<uint8_t>>* out);

    // Air datagram -> original payloads when a block can be decoded. Every
    // datagram must carry the 4-byte FEC shard header.
    void push_air(const uint8_t* data, size_t len,
                  std::vector<std::vector<uint8_t>>* out);

    uint64_t recovered() const
    {
        return recovered_;
    }
    uint64_t blocks() const
    {
        return blocks_;
    }
    uint64_t decode_fail() const
    {
        return decode_fail_;
    }
    uint64_t oversized() const
    {
        return oversized_;
    }
    // Interval since last take. recovered() / decode_fail() stay lifetime.
    uint64_t take_recovered();
    uint64_t take_decode_fail();
    /* Shards still missing when an RX block is evicted / expires (interval take). */
    uint64_t take_fail_missing_shards();
    uint64_t take_fail_lost_app_pkts();

    // Encode one block. packets.size() may be < k (tail slots are virtual pads).
    // Empty systematic slots (index >= packets.size()) are not sent; the receiver
    // inserts zero-length bodies before decode. Non-empty systematic shards omit
    // trailing zeros on the wire; parity is full width.
    bool encode_block(const std::vector<std::vector<uint8_t>>& packets,
                      uint16_t block_id,
                      std::vector<std::vector<uint8_t>>* out) const;
    // Decode from shard index -> body. k/n come from the wire header (or
    // from init() via the overload). Returns false if unrecoverable.
    bool decode_block(int k, int n,
                      const std::unordered_map<int, std::vector<uint8_t>>& frags,
                      std::vector<std::vector<uint8_t>>* payloads,
                      int* recovered) const;
    bool decode_block(
        const std::unordered_map<int, std::vector<uint8_t>>& frags,
        std::vector<std::vector<uint8_t>>* payloads, int* recovered) const;

    static size_t max_original();

    static bool pack_header(uint8_t* out, uint16_t block_id, int index, int k, int n,
                            uint8_t flags, int sdu_n);
    static bool unpack_header(const uint8_t* data, size_t len, uint16_t* block_id,
                              int* index, int* k, int* n, uint8_t* flags, int* sdu_n);

private:
    struct rx_block_s
    {
        int k = 0;
        int n = 0;
        int sdu_n = 0;
        std::unordered_map<int, std::vector<uint8_t>> frags;
        std::chrono::steady_clock::time_point first_seen{};
        std::chrono::steady_clock::time_point last_seen{};
    };

    void note_rx_block_loss(const rx_block_s& block);
    void note_rx_block_output_shortfall(int expected, int available);
    static int expected_sdus(const rx_block_s& block);
    /* Emit systematic app payloads present without RS decode; gap += (N - available). */
    void finish_block_with_available(const rx_block_s& block, uint16_t block_id,
                                     std::vector<std::vector<uint8_t>>* out);
    void expire_rx(std::vector<std::vector<uint8_t>>* out);
    void expire_done();
    void mark_done(uint16_t block_id);
    void note_emit_base(uint16_t block_id);
    // Signed wrap-aware distance of block_id from emit_next.
    int emit_distance(uint16_t block_id) const;
    void prune_emit_behind();
    void advance_emit_past_hole();
    void skip_emit_block(uint16_t block_id);
    void queue_decoded_block(uint16_t block_id,
                             std::vector<std::vector<uint8_t>> payloads,
                             std::vector<std::vector<uint8_t>>* out);
    void drain_emit_queue(std::vector<std::vector<uint8_t>>* out);
    std::chrono::steady_clock::time_point now() const;
    static int gen_decode_matrix(int k, int n, const uint8_t* encode_matrix,
                                 const uint8_t* err_list, int nerrs,
                                 uint8_t* decode_matrix, uint8_t* decode_index);

    bool enabled_ = false;
    int k_ = 0;
    int n_ = 0;
    int p = 0;
    int timeout_ms = k_default_timeout_ms;
    std::vector<uint8_t> encode_matrix;
    std::vector<uint8_t> g_tbls;

    std::vector<std::vector<uint8_t>> pending;
    std::chrono::steady_clock::time_point deadline{};
    bool deadline_set = false;
    uint16_t block_id = 0;

    std::unordered_map<uint16_t, rx_block_s> rx_blocks;
    std::deque<uint16_t> done_order;
    std::unordered_map<uint16_t, std::chrono::steady_clock::time_point> done;

    std::unordered_map<uint16_t, std::vector<std::vector<uint8_t>>> emit_pending;
    std::unordered_set<uint16_t>                                    emit_skipped;
    bool     emit_base_set = false;
    uint16_t emit_next = 0;
    bool     emit_waiting = false;
    std::chrono::steady_clock::time_point emit_wait_since{};

    uint64_t recovered_ = 0;
    uint64_t recovered_seen_ = 0;
    uint64_t blocks_ = 0;
    uint64_t decode_fail_ = 0;
    uint64_t decode_fail_seen_ = 0;
    uint64_t fail_missing_shards_ = 0;
    uint64_t fail_missing_shards_seen_ = 0;
    uint64_t fail_lost_app_pkts_ = 0;
    uint64_t fail_lost_app_pkts_seen_ = 0;
    uint64_t oversized_ = 0;
};

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_RS_BLOCK_ERASURE_HPP
