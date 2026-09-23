#ifndef VSTREAMER_CORE_PACKET_POOL_HPP
#define VSTREAMER_CORE_PACKET_POOL_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "core/data_packet.hpp"

namespace vstreamer
{

class packet_pool
{
   public:
    static constexpr size_t k_default_depth = 64;

    explicit packet_pool(size_t buffer_bytes, size_t depth = k_default_depth);
    ~packet_pool();

    packet_pool(const packet_pool &) = delete;
    packet_pool &operator=(const packet_pool &) = delete;

    [[nodiscard]] uint8_t *acquire(size_t need);

    int checkout_sock(data_packet *out, int64_t pts, size_t size);

    static void release(uint8_t *data);

    [[nodiscard]] size_t buffer_bytes() const { return buf_bytes; }
    [[nodiscard]] size_t depth() const { return pool_depth; }
    [[nodiscard]] size_t available() const;

   private:
    struct block_hdr
    {
        packet_pool *pool;
        size_t       capacity;
    };

    static constexpr size_t hdr_bytes();
    static block_hdr *hdr_of(uint8_t *data) noexcept;
    void              recycle(uint8_t *data);

    size_t buf_bytes;
    size_t pool_depth;

    mutable std::mutex                      mu;
    std::vector<std::unique_ptr<uint8_t[]>> blocks;
    std::vector<uint8_t *>                  free_list;
};

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_PACKET_POOL_HPP
