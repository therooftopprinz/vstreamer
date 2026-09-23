#include "core/packet_pool.hpp"

#include <cerrno>
#include <memory>
#include <new>

namespace vstreamer
{

constexpr size_t packet_pool::hdr_bytes()
{
    constexpr size_t align = alignof(std::max_align_t);
    return (sizeof(block_hdr) + align - 1) & ~(align - 1);
}

packet_pool::block_hdr *packet_pool::hdr_of(uint8_t *data) noexcept
{
    return reinterpret_cast<block_hdr *>(data - hdr_bytes());
}

packet_pool::packet_pool(size_t buffer_bytes, size_t depth)
    : buf_bytes(buffer_bytes), pool_depth(depth)
{
    blocks.reserve(pool_depth);
    free_list.reserve(pool_depth);

    const size_t total = hdr_bytes() + buf_bytes;
    for (size_t i = 0; i < pool_depth; ++i)
    {
        auto mem = std::make_unique<uint8_t[]>(total);
        auto *hdr = reinterpret_cast<block_hdr *>(mem.get());
        hdr->pool = this;
        hdr->capacity = buf_bytes;
        uint8_t *user = mem.get() + hdr_bytes();
        free_list.push_back(user);
        blocks.push_back(std::move(mem));
    }
}

packet_pool::~packet_pool()
{
    std::lock_guard<std::mutex> lock(mu);
    free_list.clear();
    for (auto &block : blocks)
    {
        auto *hdr = reinterpret_cast<block_hdr *>(block.get());
        hdr->pool = nullptr;
    }
    blocks.clear();
}

uint8_t *packet_pool::acquire(size_t need)
{
    if (need > buf_bytes)
    {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(mu);
    if (free_list.empty())
    {
        return nullptr;
    }

    uint8_t *p = free_list.back();
    free_list.pop_back();
    return p;
}

int packet_pool::checkout_sock(data_packet *out, int64_t pts, size_t size)
{
    if (nullptr == out)
    {
        return -EINVAL;
    }

    uint8_t *buf = acquire(size);
    if (nullptr == buf)
    {
        return -ENOMEM;
    }

    auto sd = std::make_unique<sock_data>();
    sd->pts = pts;
    sd->buf.reset(buf, size, &packet_pool::release);
    out->reset(std::move(sd));
    return 0;
}

void packet_pool::release(uint8_t *data)
{
    if (nullptr == data)
    {
        return;
    }

    block_hdr *hdr = hdr_of(data);
    packet_pool *pool = hdr->pool;
    if (nullptr == pool)
    {
        return;
    }
    pool->recycle(data);
}

void packet_pool::recycle(uint8_t *data)
{
    std::lock_guard<std::mutex> lock(mu);
    free_list.push_back(data);
}

size_t packet_pool::available() const
{
    std::lock_guard<std::mutex> lock(mu);
    return free_list.size();
}

}  // namespace vstreamer
