#include "core/packet_types.hpp"

#include <utility>

namespace vstreamer
{

void buffer_block::release()
{
    if (nullptr != data && deleter)
    {
        deleter(data);
    }
    data = nullptr;
    size = 0;
    deleter = nullptr;
}

void buffer_block::reset(uint8_t *ptr, size_t nbytes, std::function<void(uint8_t *)> d)
{
    release();
    data = ptr;
    size = nbytes;
    deleter = std::move(d);
}

buffer_block::buffer_block(buffer_block &&other) noexcept
    : data(other.data), size(other.size), deleter(std::move(other.deleter))
{
    other.data = nullptr;
    other.size = 0;
}

buffer_block &buffer_block::operator=(buffer_block &&other) noexcept
{
    if (this != &other)
    {
        release();
        data = other.data;
        size = other.size;
        deleter = std::move(other.deleter);
        other.data = nullptr;
        other.size = 0;
    }
    return *this;
}

buffer_block::~buffer_block()
{
    release();
}

}  // namespace vstreamer
