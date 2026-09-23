#ifndef VSTREAMER_CORE_DATA_PACKET_HPP
#define VSTREAMER_CORE_DATA_PACKET_HPP

#include <cstdint>
#include <memory>
#include <utility>

#include "core/frame.hpp"
#include "core/packet_error.hpp"
#include "core/packet_kind.hpp"
#include "core/packet_types.hpp"

namespace vstreamer
{

class data_packet
{
public:
    data_packet() = default;

    explicit data_packet(std::unique_ptr<packet_body> body_in) : body(std::move(body_in)) {}

    data_packet(const data_packet &) = delete;
    data_packet &operator=(const data_packet &) = delete;

    data_packet(data_packet &&other) noexcept = default;
    data_packet &operator=(data_packet &&other) noexcept = default;

    ~data_packet() = default;

    void release() { body.reset(); }

    [[nodiscard]] bool empty() const { return nullptr == body; }

    [[nodiscard]] packet_kind_e get_type() const
    {
        if (nullptr == body)
        {
            return packet_kind_e::UNKNOWN;
        }
        return body->get_type();
    }

    [[nodiscard]] packet_body &get()
    {
        if (nullptr == body)
        {
            throw packet_type_error("data_packet is empty");
        }
        return *body;
    }

    [[nodiscard]] const packet_body &get() const
    {
        if (nullptr == body)
        {
            throw packet_type_error("data_packet is empty");
        }
        return *body;
    }

    template <typename T>
    [[nodiscard]] static T &cast(data_packet &pkt)
    {
        if (pkt.get_type() != T::k_kind)
        {
            throw packet_type_error("data_packet type mismatch");
        }
        return static_cast<T &>(pkt.get());
    }

    template <typename T>
    [[nodiscard]] static const T &cast(const data_packet &pkt)
    {
        if (pkt.get_type() != T::k_kind)
        {
            throw packet_type_error("data_packet type mismatch");
        }
        return static_cast<const T &>(pkt.get());
    }

    void reset(std::unique_ptr<packet_body> body_in) { body = std::move(body_in); }

    void adopt_frame(frame &&fr);
    void move_to_frame(frame &out);

private:
    std::unique_ptr<packet_body> body;
};

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_DATA_PACKET_HPP
