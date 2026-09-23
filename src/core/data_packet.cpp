#include "core/data_packet.hpp"

#include <memory>
#include <utility>

namespace vstreamer
{

void data_packet::adopt_frame(frame &&fr)
{
    auto fd = std::make_unique<frame_data>();
    fd->kind = fr.fields.kind;
    fd->width = fr.fields.width;
    fd->height = fr.fields.height;
    fd->pts = fr.fields.pts;
    fd->capture_mono_ns = fr.fields.capture_mono_ns;
    fd->key = fr.fields.key;
    fd->buf.data = fr.fields.data;
    fd->buf.size = fr.fields.size;
    fd->buf.deleter = std::move(fr.fields.data_deleter);
    fr.reset_owned();
    body = std::move(fd);
}

void data_packet::move_to_frame(frame &out)
{
    frame_data &f = cast<frame_data>(*this);
    out.reset(f.kind, f.width, f.height, f.pts, f.key, f.buf.data, f.buf.size, f.buf.deleter,
              f.capture_mono_ns);
    f.buf.data = nullptr;
    f.buf.size = 0;
    f.buf.deleter = nullptr;
    release();
}

}  // namespace vstreamer
