#include "components/encoder_cbr_logic.hpp"

#include "core/key_util.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace vstreamer
{
namespace
{

bool admission_pacing_enabled_by_env()
{
    const char *env = std::getenv("VSTREAMER_ENC_ADMISSION");
    if (nullptr == env || env[0] == '\0')
    {
        return false;
    }
    return 0 != std::strcmp(env, "0");
}

}  // namespace

encoder_cbr_logic::encoder_cbr_logic() = default;

encoder_cbr_logic::~encoder_cbr_logic() = default;

void encoder_cbr_logic::set_target_kbps(int kbps)
{
    if (kbps > 0)
    {
        target_kbps = kbps;
        nominal_kbps = kbps;
        reset_cbr_pid();
        pid_cbr_kbps = static_cast<double>(kbps);
        pending_cbr_kbps.store(kbps, std::memory_order_relaxed);
    }
}

void encoder_cbr_logic::reset_cbr_pid()
{
    pid_integral = 0.;
    pid_prev_error = 0.;
    pid_deriv_filtered = 0.;
    link_stressed_latch = false;
    pid_last_ts = std::chrono::steady_clock::time_point {};
    measured_ema_kbps = 0.f;
}

float encoder_cbr_logic::estimate_sustainable_kbps(const stream_telemetry &tel) const
{
    if (nominal_kbps <= 0)
    {
        return 0.f;
    }

    float raw = 0.f;
    if (tel.deliverable_kbps > 50.f)
    {
        raw = tel.deliverable_kbps;
    }
    else if (tel.egress_kbps > 50.f)
    {
        raw = tel.egress_kbps * (1.f - tel.channel_loss);
    }
    else
    {
        float scale = 1.f - tel.channel_loss;
        if (scale < 0.15f)
        {
            scale = 0.15f;
        }
        raw = static_cast<float>(nominal_kbps) * scale;
    }
    return raw;
}

bool encoder_cbr_logic::mpp_rc_is_cbr() const
{
    if (nullptr == enc)
    {
        return false;
    }
    std::string_view rc_mode;
    return enc->query(std::string_view("rc"), &rc_mode) == 0 && rc_mode == "cbr";
}

bool encoder_cbr_logic::admission_pacing_for_resolution() const
{
    if (nullptr == enc)
    {
        return false;
    }
    std::string_view sz;
    if (enc->query(std::string_view("size"), &sz) != 0 || sz.empty())
    {
        return false;
    }
    int w = 0;
    int h = 0;
    char buf[64];
    const size_t n = std::min(sz.size(), sizeof(buf) - 1);
    std::memcpy(buf, sz.data(), n);
    buf[n] = '\0';
    char *x = std::strchr(buf, 'x');
    if (nullptr == x)
    {
        x = std::strchr(buf, 'X');
    }
    if (nullptr == x)
    {
        return false;
    }
    *x = '\0';
    w = std::atoi(buf);
    h = std::atoi(x + 1);
    return w > 0 && h > 0 && static_cast<int64_t>(w) * static_cast<int64_t>(h) >= 1920 * 1080;
}

bool encoder_cbr_logic::admission_pacing_active() const
{
    return admission_pacing_enabled_by_env() && mpp_rc_is_cbr() &&
           admission_pacing_for_resolution();
}

void encoder_cbr_logic::wait_encode_admission()
{
    if (nullptr == enc || !admission_pacing_active())
    {
        return;
    }
    refresh_target_from_encoder();
    if (target_kbps <= 0)
    {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (next_encode_admit > now)
    {
        admit_sleep_count.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::sleep_until(next_encode_admit);
        const auto after = std::chrono::steady_clock::now();
        const auto slept_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(after - now);
        admit_sleep_ns.fetch_add(static_cast<uint64_t>(slept_ns.count()),
                                 std::memory_order_relaxed);
    }
}

void encoder_cbr_logic::note_emitted_au_bytes(size_t nbytes)
{
    if (nullptr == enc || !admission_pacing_active() || target_kbps <= 0 || nbytes == 0)
    {
        return;
    }
    refresh_target_from_encoder();
    const double target_bps = static_cast<double>(target_kbps) * 1000.0;
    const double sec = static_cast<double>(nbytes) * 8.0 / target_bps;
    const auto   gap =
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(sec));
    const auto now = std::chrono::steady_clock::now();
    next_encode_admit = now + gap;
}

void encoder_cbr_logic::refresh_target_from_encoder()
{
    if (nullptr == enc)
    {
        return;
    }
    std::string_view cbr;
    if (enc->query(std::string_view("cbr"), &cbr) == 0 && !cbr.empty())
    {
        const int bps = std::atoi(cbr.data());
        if (bps > 0)
        {
            target_kbps = (bps + 500) / 1000;
        }
    }
}

void encoder_cbr_logic::bind_encoder(component_coder *encoder)
{
    enc = encoder;
    target_kbps = 0;
    last_qp = 36;
    if (nullptr == enc)
    {
        return;
    }
    refresh_target_from_encoder();
    enc_mpp_cbr = mpp_rc_is_cbr();
    if (nominal_kbps <= 0 && target_kbps > 0)
    {
        nominal_kbps = target_kbps;
    }
    reset_cbr_pid();
    pid_cbr_kbps = static_cast<double>(target_kbps > 0 ? target_kbps : nominal_kbps);
    pending_cbr_kbps.store(static_cast<int>(std::lround(pid_cbr_kbps)),
                           std::memory_order_relaxed);
    std::string_view qp;
    if (enc->query(std::string_view("qp"), &qp) == 0 && !qp.empty())
    {
        last_qp = std::atoi(qp.data());
    }
}

int encoder_cbr_logic::update_cbr_pid_from_telemetry(const stream_telemetry &tel)
{
    if (nominal_kbps <= 0)
    {
        return 0;
    }

    if (tel.channel_loss > static_cast<float>(pid_tuning_cfg.loss_enter))
    {
        link_stressed_latch = true;
    }
    else if (tel.channel_loss < static_cast<float>(pid_tuning_cfg.loss_exit))
    {
        link_stressed_latch = false;
    }
    const bool link_stressed = link_stressed_latch;

    float raw_sustainable = estimate_sustainable_kbps(tel);
    if (!link_stressed)
    {
        raw_sustainable = static_cast<float>(nominal_kbps);
    }

    if (measured_ema_kbps <= 0.f)
    {
        measured_ema_kbps = raw_sustainable;
    }
    else if (!link_stressed)
    {
        const float b = static_cast<float>(pid_tuning_cfg.ema_recover_blend);
        measured_ema_kbps = (1.f - b) * measured_ema_kbps + b * raw_sustainable;
    }
    else if (raw_sustainable > measured_ema_kbps)
    {
        const float b = static_cast<float>(pid_tuning_cfg.ema_stress_rise_blend);
        measured_ema_kbps = (1.f - b) * measured_ema_kbps + b * raw_sustainable;
    }
    else
    {
        const float b = static_cast<float>(pid_tuning_cfg.ema_stress_fall_blend);
        measured_ema_kbps = (1.f - b) * measured_ema_kbps + b * raw_sustainable;
    }

    double setpoint = static_cast<double>(nominal_kbps);
    if (link_stressed)
    {
        setpoint = std::min(setpoint, static_cast<double>(measured_ema_kbps));
    }
    else if (pid_integral < 0.)
    {
        pid_integral *= 0.7;
    }

    if (pid_cbr_kbps <= 0.)
    {
        refresh_target_from_encoder();
        pid_cbr_kbps = static_cast<double>(target_kbps > 0 ? target_kbps : nominal_kbps);
    }

    const auto now = std::chrono::steady_clock::now();
    double     dt = 0.1;
    if (pid_last_ts != std::chrono::steady_clock::time_point {})
    {
        dt = std::chrono::duration<double>(now - pid_last_ts).count();
    }
    pid_last_ts = now;
    if (dt < 0.05)
    {
        dt = 0.05;
    }
    if (dt > 1.0)
    {
        dt = 1.0;
        pid_integral = 0.;
    }

    const double error = setpoint - pid_cbr_kbps;
    pid_integral += error * dt;
    const double i_limit =
        static_cast<double>(nominal_kbps) * pid_tuning_cfg.i_limit_ratio;
    pid_integral = std::clamp(pid_integral, -i_limit, i_limit);

    const double derivative = (error - pid_prev_error) / dt;
    pid_prev_error = error;
    const double da = pid_tuning_cfg.deriv_filter_alpha;
    pid_deriv_filtered = (1.0 - da) * pid_deriv_filtered + da * derivative;

    double delta = pid_tuning_cfg.kp * error + pid_tuning_cfg.ki * pid_integral +
                   pid_tuning_cfg.kd * pid_deriv_filtered;

    const double step_down =
        static_cast<double>(nominal_kbps) / pid_tuning_cfg.step_down_divisor;
    const double step_up = static_cast<double>(nominal_kbps) / pid_tuning_cfg.step_up_divisor;
    if (delta < 0.)
    {
        delta = std::max(delta, -step_down);
    }
    else
    {
        delta = std::min(delta, step_up);
    }

    pid_cbr_kbps = std::clamp(pid_cbr_kbps + delta, 100., static_cast<double>(nominal_kbps));

    const int new_kbps = static_cast<int>(std::lround(pid_cbr_kbps));
    pending_cbr_kbps.store(new_kbps, std::memory_order_relaxed);
    return 0;
}

int encoder_cbr_logic::apply_pending_cbr_to_encoder()
{
    const int new_kbps = pending_cbr_kbps.load(std::memory_order_relaxed);
    if (new_kbps < 0 || nullptr == enc)
    {
        return 0;
    }

    refresh_target_from_encoder();
    const int current_kbps = target_kbps > 0 ? target_kbps : nominal_kbps;
    const int deadband = std::max(25, nominal_kbps / 200);
    if (std::abs(new_kbps - current_kbps) < deadband)
    {
        return 0;
    }

    const auto now = std::chrono::steady_clock::now();
    if (last_cbr_apply != std::chrono::steady_clock::time_point {} &&
        now - last_cbr_apply < std::chrono::milliseconds(pid_tuning_cfg.apply_min_ms))
    {
        return 0;
    }

    char buf[24];
    std::snprintf(buf, sizeof(buf), "%d", new_kbps * 1000);
    std::string_view v(buf);
    const int        r = enc->configure(std::string_view("cbr"), &v);
    if (r == 0)
    {
        last_cbr_apply = now;
        target_kbps = new_kbps;
        pid_cbr_kbps = static_cast<double>(new_kbps);
    }
    else
    {
        static std::atomic<int> log_left {5};
        if (log_left.fetch_sub(1, std::memory_order_relaxed) >= 0)
        {
            std::fprintf(stderr, "encoder_cbr_logic: set cbr %d kb/s failed (%d)\n", new_kbps, r);
        }
    }
    return r;
}

void encoder_cbr_logic::flush_pending_cbr_to_encoder()
{
    if (!enc_mpp_cbr)
    {
        return;
    }
    (void)apply_pending_cbr_to_encoder();
}

int encoder_cbr_logic::apply(const stream_telemetry &tel)
{
    if (nullptr == enc)
    {
        return -EINVAL;
    }

    frame_skip_stride.store(1, std::memory_order_relaxed);

    if (enc_mpp_cbr)
    {
        return update_cbr_pid_from_telemetry(tel);
    }

    refresh_target_from_encoder();

    if (target_kbps <= 0 || tel.egress_kbps <= 0.f)
    {
        return 0;
    }

    const float ratio = tel.egress_kbps / static_cast<float>(target_kbps);
    int         qp = last_qp;

    if (ratio > 1.08f)
    {
        qp += ratio > 2.f ? 5 : (ratio > 1.35f ? 3 : 2);
    }
    else if (ratio < 0.92f)
    {
        qp -= ratio < 0.80f ? 3 : (ratio < 0.88f ? 2 : 1);
    }
    else if (ratio < 0.98f)
    {
        qp -= 1;
    }

    if (ratio > 1.2f || ratio < 0.8f)
    {
        if (tel.flow > 0.75f)
        {
            qp += 1;
        }
        if (tel.channel_loss > 0.1f)
        {
            qp += 1;
        }
    }

    if (qp < qp_floor)
    {
        qp = qp_floor;
    }
    if (qp > qp_ceiling)
    {
        qp = qp_ceiling;
    }
    if (qp == last_qp)
    {
        return 0;
    }

    const auto now = std::chrono::steady_clock::now();
    if (last_qp_apply != std::chrono::steady_clock::time_point {} &&
        now - last_qp_apply < std::chrono::milliseconds(250))
    {
        return 0;
    }

    last_qp = qp;
    last_qp_apply = now;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d", qp);
    std::string_view v(buf);
    return enc->configure(std::string_view("qp"), &v);
}

void encoder_cbr_logic::set_pid_tuning(const cbr_pid_tuning &tuning)
{
    pid_tuning_cfg = tuning;
}

cbr_pid_tuning encoder_cbr_logic::pid_tuning() const
{
    return pid_tuning_cfg;
}

void encoder_cbr_logic::feed_telemetry_for_pid(const stream_telemetry &tel)
{
    if (nominal_kbps <= 0)
    {
        return;
    }
    enc_mpp_cbr = true;
    (void)update_cbr_pid_from_telemetry(tel);
}

cbr_pid_runtime_state encoder_cbr_logic::pid_runtime_state() const
{
    cbr_pid_runtime_state s;
    s.pending_kbps = pending_cbr_kbps.load(std::memory_order_relaxed);
    s.nominal_kbps = nominal_kbps;
    s.measured_ema_kbps = measured_ema_kbps;
    s.pid_cbr_kbps = pid_cbr_kbps;
    s.pid_integral = pid_integral;
    s.link_stressed = link_stressed_latch;
    return s;
}

namespace
{

bool parse_kv_double(const char *token, const char *key, double *out)
{
    const size_t key_len = std::strlen(key);
    if (std::strncmp(token, key, key_len) != 0 || token[key_len] != '=')
    {
        return false;
    }
    char       *end = nullptr;
    const double v = std::strtod(token + key_len + 1, &end);
    if (end == token + key_len + 1)
    {
        return false;
    }
    *out = v;
    return true;
}

bool parse_kv_int(const char *token, const char *key, int *out)
{
    double v = 0.;
    if (!parse_kv_double(token, key, &v))
    {
        return false;
    }
    *out = static_cast<int>(v);
    return true;
}

}  // namespace

void encoder_cbr_logic::handle_pid_console_line(const char *line, char *reply, size_t reply_cap)
{
    if (nullptr == line || nullptr == reply || reply_cap == 0)
    {
        return;
    }

    if (0 == std::strcmp(line, "cbr_pid") || 0 == std::strcmp(line, "get_cbr_pid"))
    {
        const cbr_pid_runtime_state rt = pid_runtime_state();
        std::snprintf(reply, reply_cap,
                      "nominal=%d pending=%d ema=%.0f pid=%.0f integral=%.0f stressed=%d\n"
                      "kp=%.4f ki=%.4f kd=%.4f deriv_a=%.3f i_lim=%.3f\n"
                      "step_down=1/%.0f step_up=1/%.0f loss_in=%.3f loss_out=%.3f "
                      "apply_ms=%d\n",
                      rt.nominal_kbps, rt.pending_kbps, static_cast<double>(rt.measured_ema_kbps),
                      rt.pid_cbr_kbps, rt.pid_integral, rt.link_stressed ? 1 : 0,
                      pid_tuning_cfg.kp, pid_tuning_cfg.ki, pid_tuning_cfg.kd,
                      pid_tuning_cfg.deriv_filter_alpha, pid_tuning_cfg.i_limit_ratio,
                      pid_tuning_cfg.step_down_divisor, pid_tuning_cfg.step_up_divisor,
                      pid_tuning_cfg.loss_enter, pid_tuning_cfg.loss_exit,
                      pid_tuning_cfg.apply_min_ms);
        return;
    }

    if (0 == std::strncmp(line, "set_cbr_pid ", 12))
    {
        const char *args = line + 12;
        if (0 == std::strcmp(args, "defaults") || 0 == std::strcmp(args, "reset"))
        {
            pid_tuning_cfg = cbr_pid_tuning_defaults();
            reset_cbr_pid();
            if (nominal_kbps > 0)
            {
                pid_cbr_kbps = static_cast<double>(nominal_kbps);
                pending_cbr_kbps.store(nominal_kbps, std::memory_order_relaxed);
            }
            std::snprintf(reply, reply_cap, "ok\n");
            return;
        }

        cbr_pid_tuning next = pid_tuning_cfg;
        char           buf[256];
        std::snprintf(buf, sizeof(buf), "%s", args);
        char *save = nullptr;
        for (char *tok = strtok_r(buf, " \t", &save); nullptr != tok;
             tok = strtok_r(nullptr, " \t", &save))
        {
            double dv = 0.;
            int    iv = 0;
            if (parse_kv_double(tok, "kp", &dv))
            {
                next.kp = dv;
            }
            else if (parse_kv_double(tok, "ki", &dv))
            {
                next.ki = dv;
            }
            else if (parse_kv_double(tok, "kd", &dv))
            {
                next.kd = dv;
            }
            else if (parse_kv_double(tok, "deriv_a", &dv))
            {
                next.deriv_filter_alpha = dv;
            }
            else if (parse_kv_double(tok, "i_lim", &dv))
            {
                next.i_limit_ratio = dv;
            }
            else if (parse_kv_double(tok, "step_down", &dv) ||
                     parse_kv_double(tok, "step_stress", &dv))
            {
                next.step_down_divisor = dv;
            }
            else if (parse_kv_double(tok, "step_up", &dv) ||
                     parse_kv_double(tok, "step_recover", &dv))
            {
                next.step_up_divisor = dv;
            }
            else if (parse_kv_double(tok, "loss_in", &dv))
            {
                next.loss_enter = dv;
            }
            else if (parse_kv_double(tok, "loss_out", &dv))
            {
                next.loss_exit = dv;
            }
            else if (parse_kv_int(tok, "apply_ms", &iv))
            {
                next.apply_min_ms = iv;
            }
            else if (parse_kv_double(tok, "ema_recover", &dv))
            {
                next.ema_recover_blend = dv;
            }
            else if (parse_kv_double(tok, "ema_stress_up", &dv))
            {
                next.ema_stress_rise_blend = dv;
            }
            else if (parse_kv_double(tok, "ema_stress_dn", &dv))
            {
                next.ema_stress_fall_blend = dv;
            }
            else
            {
                std::snprintf(reply, reply_cap, "err unknown key %s\n", tok);
                return;
            }
        }
        pid_tuning_cfg = next;
        std::snprintf(reply, reply_cap, "ok\n");
        return;
    }

    std::snprintf(reply, reply_cap, "err unknown\n");
}

int encoder_cbr_logic::configure(uint64_t /*key*/, int64_t /*value*/)
{
    return -ENOTSUP;
}

int encoder_cbr_logic::query(uint64_t /*key*/, int64_t * /*value*/) const
{
    return -ENOTSUP;
}

int encoder_cbr_logic::configure(std::string_view key, std::string_view *value)
{
    if (nullptr == value)
    {
        return -EINVAL;
    }
    if ("qp_floor" == key || "qp_ceiling" == key)
    {
        int64_t v = 0;
        char    buf[32];
        std::memcpy(buf, value->data(), value->size());
        buf[value->size()] = '\0';
        if (key_parse_i64(buf, &v) < 0 || v < 1 || v > 51)
        {
            return -EINVAL;
        }
        if ("qp_floor" == key)
        {
            qp_floor = static_cast<int>(v);
        }
        else
        {
            qp_ceiling = static_cast<int>(v);
        }
        return 0;  
    }
    return -ENOTSUP;
}

int encoder_cbr_logic::query(std::string_view /*key*/, std::string_view * /*value*/) const
{
    return -ENOTSUP;
}

}  // namespace vstreamer
