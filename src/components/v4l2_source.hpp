#ifndef VSTREAMER_SOURCE_V4L2_SOURCE_HPP
#define VSTREAMER_SOURCE_V4L2_SOURCE_HPP

#include "components/components_config.hpp"
#ifndef ENABLE_V4L2_SOURCE
#error "v4l2_source requires -DENABLE_V4L2_SOURCE=ON"
#endif
#ifndef ENABLE_NOISE_SOURCE
#error "v4l2_source requires -DENABLE_NOISE_SOURCE=ON"
#endif

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "core/component_source.hpp"
#include "components/noise_source.hpp"

namespace vstreamer
{

/* UVC MJPEG mmap capture (rover camera path). Falls back to noise_source. */
class v4l2_source : public component_source
{
   public:
    static constexpr unsigned k_nbufs = 8;

    v4l2_source();
    ~v4l2_source() override;

    v4l2_source(const v4l2_source &) = delete;
    v4l2_source &operator=(const v4l2_source &) = delete;

    [[nodiscard]] std::string name() const override;
    [[nodiscard]] media_kind_e output_kind() const override;

    int  open() override;
    void close() override;

    int output(uint8_t port, data_packet &out, int timeout_ms) override;

    int configure(uint64_t key, int64_t value) override;
    int query(uint64_t key, int64_t *value) const override;

    int configure(std::string_view key, std::string_view *value) override;
    int query(std::string_view key, std::string_view *value) const override;

   private:
    int  capture_open_locked(bool log_fail);
    void capture_close_locked();
    int  apply_ctrl_stash_locked();
    int  set_ctrl_locked(uint32_t id, int32_t value);
    int  get_ctrl_locked(uint32_t id, int64_t *value) const;
    int  resolve_ctrl_name_locked(std::string_view name, uint32_t *id) const;
    int  list_ctrls_locked(std::string *out) const;
    int  fetch_live_locked(frame &out, int timeout_ms);
    bool maybe_retry_capture_locked();

    mutable std::mutex mu;

    std::string device = "/dev/video0";
    std::string format = "mjpeg";
    int         width = 1280;
    int         height = 720;
    int         fps = 30;

    int                 fd = -1;
    bool                capture_open = false;
    bool                source_open = false;
    unsigned            nbufs = 0;
    std::vector<void *> maps;
    std::vector<size_t> lengths;

    int     live_w = 0;
    int     live_h = 0;
    int64_t pts = 0;

    bool   noise_active = false;
    double cap_retry_due = 0;

    /* CID -> value; applied after STREAMON / open. */
    std::map<uint32_t, int32_t> ctrl_stash;

    noise_source noise;

    mutable std::string query_buf;
};

}  // namespace vstreamer

#endif  // VSTREAMER_SOURCE_V4L2_SOURCE_HPP
