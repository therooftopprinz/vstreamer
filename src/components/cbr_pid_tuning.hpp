#ifndef VSTREAMER_COMPONENTS_CBR_PID_TUNING_HPP
#define VSTREAMER_COMPONENTS_CBR_PID_TUNING_HPP

namespace vstreamer
{

/* Defaults: damped PI-D on ~100 ms telemetry; avoids hunting on noisy goodput. */
struct cbr_pid_tuning
{
    double kp = 0.06;
    double ki = 0.015;
    double kd = 0.30;
    double deriv_filter_alpha = 0.18;
    double i_limit_ratio = 0.10;
    /* Max kb/s change per telemetry tick: nominal/divisor (smaller div = bigger step). */
    double step_down_divisor = 18.;
    double step_up_divisor = 90.;
    double loss_enter = 0.045;
    double loss_exit = 0.015;
    int    apply_min_ms = 1500;
    double ema_recover_blend = 0.12;
    double ema_stress_rise_blend = 0.20;
    double ema_stress_fall_blend = 0.10;
};

[[nodiscard]] inline cbr_pid_tuning cbr_pid_tuning_defaults()
{
    return cbr_pid_tuning {};
}

}  // namespace vstreamer

#endif  // VSTREAMER_COMPONENTS_CBR_PID_TUNING_HPP
