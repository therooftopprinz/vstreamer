#ifndef VSTREAMER_CORE_FRAME_POOL_HPP
#define VSTREAMER_CORE_FRAME_POOL_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "core/frame.hpp"

namespace vstreamer
{

/*
 * Fixed-size buffer pool for frame payloads.
 * acquire() hands out a buffer; pass &frame_pool::release as frame::reset
 * deleter so the buffer returns here when the frame is destroyed/released.
 * The pool must outlive every outstanding buffer.
 */
class frame_pool
{
   public:
    static constexpr size_t k_default_depth = 32;

    explicit frame_pool(size_t buffer_bytes, size_t depth = k_default_depth);
    ~frame_pool();

    frame_pool(const frame_pool &) = delete;
    frame_pool &operator=(const frame_pool &) = delete;

    /* Buffer with capacity >= need, or nullptr if exhausted / too large. */
    [[nodiscard]] uint8_t *acquire(size_t need);

    /*
     * Acquire a buffer of `size` bytes and attach it to `out` with pool
     * recycling. Caller fills out->data()[0..size). Returns 0, -ENOMEM
     * if the pool is empty, or -EINVAL.
     */
    int checkout(frame *out, media_kind_e kind, int width, int height, int64_t pts,
                 bool key, size_t size);

    /* Return a buffer from acquire/checkout. No-op on nullptr. */
    static void release(uint8_t *data);

    [[nodiscard]] size_t buffer_bytes() const { return buf_bytes; }
    [[nodiscard]] size_t depth() const { return pool_depth; }
    [[nodiscard]] size_t available() const;

   private:
    struct block_hdr
    {
        frame_pool *pool;
        size_t      capacity;
    };

    static constexpr size_t hdr_bytes();
    static block_hdr *hdr_of(uint8_t *data) noexcept;
    void              recycle(uint8_t *data);

    size_t buf_bytes;
    size_t pool_depth;

    mutable std::mutex               mu;
    std::vector<std::unique_ptr<uint8_t[]>> blocks;
    std::vector<uint8_t *>           free_list;
};

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_FRAME_POOL_HPP
