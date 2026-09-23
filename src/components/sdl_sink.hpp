#ifndef VSTREAMER_COMPONENTS_SDL_SINK_HPP
#define VSTREAMER_COMPONENTS_SDL_SINK_HPP

#include "components/components_config.hpp"
#ifndef ENABLE_SDL_SINK
#error "sdl_sink requires -DENABLE_SDL_SINK=ON"
#endif

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

#include "components/sdl_nv12_presenter.hpp"
#include "core/component_sink.hpp"
#include "core/data_packet.hpp"

namespace vstreamer
{

/* Packed NV12 preview (GS path after h264_decoder_mpp). */
class sdl_sink : public component_sink
{
public:
    sdl_sink();
    ~sdl_sink() override;

    sdl_sink(const sdl_sink &) = delete;
    sdl_sink &operator=(const sdl_sink &) = delete;

    [[nodiscard]] std::string name() const override;
    [[nodiscard]] media_kind_e input_kind() const override;

    int  open() override;
    void close() override;

    int prepare(int width, int height);

    int input(uint8_t port, const data_packet &in) override;

    int configure(uint64_t key, int64_t value) override;
    int query(uint64_t key, int64_t *value) const override;

    int configure(std::string_view key, std::string_view *value) override;
    int query(std::string_view key, std::string_view *value) const override;

private:
    mutable std::mutex mu;

    bool opened = false;
    uint64_t frames_in = 0;

    sdl_nv12_presenter present {"sdl_sink", nullptr};

    mutable std::string query_buf;
};

}  // namespace vstreamer

#endif  // VSTREAMER_COMPONENTS_SDL_SINK_HPP
