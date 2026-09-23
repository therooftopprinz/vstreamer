#ifndef VSTREAMER_CORE_PACKET_TYPES_HPP
#define VSTREAMER_CORE_PACKET_TYPES_HPP

#include <cstddef>
#include <cstdint>
#include <functional>

#include "core/frame.hpp"
#include "core/packet_kind.hpp"

namespace vstreamer
{

struct buffer_block
{
    uint8_t *data = nullptr;
    size_t   size = 0;
    std::function<void(uint8_t *)> deleter;

    void release();
    void reset(uint8_t *ptr, size_t nbytes, std::function<void(uint8_t *)> d);

    buffer_block() = default;
    buffer_block(buffer_block &&other) noexcept;
    buffer_block &operator=(buffer_block &&other) noexcept;
    ~buffer_block();

    buffer_block(const buffer_block &) = delete;
    buffer_block &operator=(const buffer_block &) = delete;
};

class packet_body
{
public:
    virtual ~packet_body() = default;

    [[nodiscard]] virtual packet_kind_e get_type() const = 0;
};

/* Video or still image (MJPEG, NV12, H.264, …). */
class frame_data : public packet_body
{
public:
    static constexpr packet_kind_e k_kind = packet_kind_e::FRAME;

    [[nodiscard]] packet_kind_e get_type() const override { return k_kind; }

    media_kind_e kind = media_kind_e::UNKNOWN;
    int          width = 0;
    int          height = 0;
    int64_t      pts = 0;
    /* Source acquisition time (steady_clock ns); 0 if unset. */
    int64_t      capture_mono_ns = 0;
    bool         key = false;
    buffer_block buf;
};

/* Placeholder for future PCM / compressed audio. */
class audio_data : public packet_body
{
public:
    static constexpr packet_kind_e k_kind = packet_kind_e::AUDIO;

    [[nodiscard]] packet_kind_e get_type() const override { return k_kind; }

    int64_t pts = 0;
    int     sample_rate = 0;
    int     channels = 0;
    buffer_block buf;
};

/* Opaque bytes on a stream socket (e.g. one UDP datagram). */
class sock_data : public packet_body
{
public:
    static constexpr packet_kind_e k_kind = packet_kind_e::SOCK;

    [[nodiscard]] packet_kind_e get_type() const override { return k_kind; }

    int64_t pts = 0;
    buffer_block buf;
};

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_PACKET_TYPES_HPP
