#ifndef VSTREAMER_CORE_FRAME_HPP
#define VSTREAMER_CORE_FRAME_HPP

#include <cstddef>
#include <cstdint>
#include <functional>

namespace vstreamer
{

enum class media_kind_e : uint8_t
{
    UNKNOWN = 0,
    MJPEG   = 1,  // JPEG access unit
    NV12    = 2,  // packed, linesize == width
    H264    = 3,  // Annex-B access unit
};

inline void default_data_deleter(uint8_t *data)
{
    free(data);
}

class data_packet;

class frame
{
public:
    frame();
    ~frame();

    frame(const frame &) = delete;
    frame &operator=(const frame &) = delete;

    frame(frame &&other) noexcept;
    frame &operator=(frame &&other) noexcept;

    void release();

    void reset(media_kind_e kind, int width, int height, int64_t pts, bool key, uint8_t *data, size_t size, std::function<void(uint8_t *)> data_deleter = default_data_deleter, int64_t capture_mono_ns = 0);

    [[nodiscard]] media_kind_e kind() const { return fields.kind; }
    [[nodiscard]] int          width() const { return fields.width; }
    [[nodiscard]] int          height() const { return fields.height; }
    [[nodiscard]] int64_t      pts() const { return fields.pts; }
    [[nodiscard]] int64_t      capture_mono_ns() const { return fields.capture_mono_ns; }
    [[nodiscard]] bool         key() const { return fields.key; }
    [[nodiscard]] uint8_t     *data() const { return fields.data; }
    [[nodiscard]] size_t       size() const { return fields.size; }

    friend class data_packet;

private:
    void reset_owned() noexcept;

    struct
    {
        media_kind_e kind;
        int          width;
        int          height;
        int64_t      pts;
        int64_t      capture_mono_ns;
        bool         key;
        uint8_t     *data;
        size_t       size;
        std::function<void(uint8_t *)> data_deleter;
    } fields;
};

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_FRAME_HPP
