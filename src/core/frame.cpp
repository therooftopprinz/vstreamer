#include "core/frame.hpp"

#include <utility>

namespace vstreamer
{

frame::frame()
    : fields{media_kind_e::UNKNOWN, 0, 0, 0, false, nullptr, 0, {}}
{
}

frame::~frame()
{
    release();
}

frame::frame(frame &&other) noexcept
    : fields{other.fields.kind,
             other.fields.width,
             other.fields.height,
             other.fields.pts,
             other.fields.key,
             other.fields.data,
             other.fields.size,
             std::move(other.fields.data_deleter)}
{
    other.reset_owned();
}

frame &frame::operator=(frame &&other) noexcept
{
    if (this != &other)
    {
        release();
        fields.kind = other.fields.kind;
        fields.width = other.fields.width;
        fields.height = other.fields.height;
        fields.pts = other.fields.pts;
        fields.key = other.fields.key;
        fields.data = other.fields.data;
        fields.size = other.fields.size;
        fields.data_deleter = std::move(other.fields.data_deleter);
        other.reset_owned();
    }
    return *this;
}

void frame::release()
{
    if (fields.data_deleter)
    {
        fields.data_deleter(fields.data);
        fields.data_deleter = nullptr;
    }
    fields.data = nullptr;
    fields.size = 0;
}

void frame::reset(media_kind_e kind, int width, int height, int64_t pts, bool key,
                  uint8_t *data, size_t size, std::function<void(uint8_t *)> data_deleter)
{
    release();
    fields.kind = kind;
    fields.width = width;
    fields.height = height;
    fields.pts = pts;
    fields.key = key;
    fields.data = data;
    fields.size = size;
    fields.data_deleter = std::move(data_deleter);
}

void frame::reset_owned() noexcept
{
    fields.data = nullptr;
    fields.size = 0;
    fields.data_deleter = nullptr;
}

}  // namespace vstreamer
