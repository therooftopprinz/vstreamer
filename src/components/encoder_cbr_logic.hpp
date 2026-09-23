#ifndef VSTREAMER_COMPONENTS_ENCODER_CBR_LOGIC_HPP
#define VSTREAMER_COMPONENTS_ENCODER_CBR_LOGIC_HPP

#include "components/components_config.hpp"
#ifndef ENABLE_ENCODER_CBR_LOGIC
#error "encoder_cbr_logic requires -DENABLE_ENCODER_CBR_LOGIC=ON"
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

#include "core/component.hpp"
#include "core/component_coder.hpp"
#include "components/cbr_pid_tuning.hpp"
#include "core/stream_telemetry.hpp"

#include <cstddef>

namespace vstreamer
{

struct cbr_pid_runtime_state
{
    int   pending_kbps = 0;
    int   nominal_kbps = 0;
    float measured_ema_kbps = 0.f;
    double pid_cbr_kbps = 0.;
    double pid_integral = 0.;
    bool  link_stressed = false;
};

/*
 * Consumes stream_sender pad 1 (stream_telemetry).
 * Wired by the pipeline core: stream_sender.1 → encoder_cbr_logic → h264_encoder (qp / rate).
 */
class encoder_cbr_logic : public component
{
public:
    encoder_cbr_logic();
    ~encoder_cbr_logic() override;

    void bind_encoder(component_coder *encoder);

    void set_target_kbps(int kbps);
    void refresh_target_from_encoder();

    int apply(const stream_telemetry &tel);

    /* Apply pending CBR target on the encode thread (never call from telemetry). */
    void flush_pending_cbr_to_encoder();

    /* Optional bench pacing (VSTREAMER_ENC_ADMISSION=1, MPP rc=cbr, 1080p+). Default off. */
    void wait_encode_admission();

    /* Schedule next admit from emitted AU bytes and configured CBR target. */
    void note_emitted_au_bytes(size_t nbytes);

    [[nodiscard]] uint64_t admission_sleep_count() const
    {
        return admit_sleep_count.load(std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t admission_sleep_ns() const
    {
        return admit_sleep_ns.load(std::memory_order_relaxed);
    }

    /* Drop 1 of every N NV12 frames before encode when QP cannot hold target (N=1: no drop). */
    [[nodiscard]] int ingress_stride() const
    {
        return frame_skip_stride.load(std::memory_order_relaxed);
    }

    int configure(uint64_t key, int64_t value) override;
    int query(uint64_t key, int64_t *value) const override;

    int configure(std::string_view key, std::string_view *value) override;
    int query(std::string_view key, std::string_view *value) const override;

    void set_pid_tuning(const cbr_pid_tuning &tuning);
    [[nodiscard]] cbr_pid_tuning pid_tuning() const;

    /* UDP console: "set_cbr_pid kp=0.06 ...", "defaults", "reset"; "cbr_pid" shows state. */
    void handle_pid_console_line(const char *line, char *reply, size_t reply_cap);

    /* Open-loop PID steps for tests (no encoder required). */
    void feed_telemetry_for_pid(const stream_telemetry &tel);
    [[nodiscard]] cbr_pid_runtime_state pid_runtime_state() const;

private:
    [[nodiscard]] bool mpp_rc_is_cbr() const;
    [[nodiscard]] bool admission_pacing_for_resolution() const;
    [[nodiscard]] bool admission_pacing_active() const;

    void reset_cbr_pid();
    [[nodiscard]] float estimate_sustainable_kbps(const stream_telemetry &tel) const;
    int update_cbr_pid_from_telemetry(const stream_telemetry &tel);
    int apply_pending_cbr_to_encoder();

    component_coder *enc = nullptr;
    bool             enc_mpp_cbr = false;
    int              qp_floor = 20;
    int              qp_ceiling = 51;
    int              last_qp = 36;
    int              target_kbps = 0;
    int              nominal_kbps = 0;
    std::atomic<int> pending_cbr_kbps {-1};
    float            measured_ema_kbps = 0.f;
    double           pid_cbr_kbps = 0.;
    double           pid_integral = 0.;
    double           pid_prev_error = 0.;
    double           pid_deriv_filtered = 0.;
    bool             link_stressed_latch = false;
    std::chrono::steady_clock::time_point pid_last_ts {};
    std::chrono::steady_clock::time_point last_qp_apply {};
    std::chrono::steady_clock::time_point last_cbr_apply {};
    std::chrono::steady_clock::time_point next_encode_admit {};
    std::atomic<int> frame_skip_stride {1};
    std::atomic<uint64_t> admit_sleep_count {0};
    std::atomic<uint64_t> admit_sleep_ns {0};

    cbr_pid_tuning pid_tuning_cfg = cbr_pid_tuning_defaults();
    mutable std::string query_buf;
};

}  // namespace vstreamer

#endif  // VSTREAMER_COMPONENTS_ENCODER_CBR_LOGIC_HPP
