#!/usr/bin/env python3
"""Implementation of scripts/cbr_controller.human."""

from __future__ import annotations

import os
import signal
import subprocess
import sys
import time
from collections.abc import Callable
from pathlib import Path

_SCRIPTS = Path(__file__).resolve().parent
if str(_SCRIPTS) not in sys.path:
    sys.path.insert(0, str(_SCRIPTS))

from utils import (  # noqa: E402
    Args,
    Clamp,
    Console,
    Delay,
    FecMap,
    FirBoxcar,
    LPF,
    Metrics,
    PID,
)
from utils.plot import PlotWS  # noqa: E402
from utils.tune_console import TuneConsole  # noqa: E402

_TUNE_HELP = """\
commands (UDP text line):
  show | status
  set <kp|ki|kd|recover-rate> <value>
  kp|ki|kd|recover-rate <value>
  reset          clear PID integral / derivative state
  help
example: echo -n 'set kp 0.35' | nc -u -w1 127.0.0.1 5092"""


def _make_tune_handler(
    cbr_pid: PID,
    recover_rate: list[float],
) -> Callable[[str], str]:
    def handle(line: str) -> str:
        parts = line.strip().split()
        if not parts:
            return "ok"
        cmd = parts[0].lower().replace("_", "-")
        if cmd in ("help", "?"):
            return _TUNE_HELP
        if cmd in ("show", "status"):
            return (
                f"kp={cbr_pid.kp} ki={cbr_pid.ki} kd={cbr_pid.kd} "
                f"recover-rate={recover_rate[0]} cbr={cbr_pid.last}"
            )
        if cmd == "reset":
            cbr_pid.reset()
            return "ok pid reset"
        key: str | None = None
        val: float | None = None
        if cmd == "set" and len(parts) >= 3:
            key = parts[1].lower().replace("_", "-")
            try:
                val = float(parts[2])
            except ValueError:
                return f"bad value: {parts[2]}"
        elif cmd in ("kp", "ki", "kd", "recover-rate") and len(parts) >= 2:
            key = cmd
            try:
                val = float(parts[1])
            except ValueError:
                return f"bad value: {parts[1]}"
        if key is not None and val is not None:
            if key == "kp":
                cbr_pid.set_gains(val, cbr_pid.ki, cbr_pid.kd)
                return f"ok kp={val}"
            if key == "ki":
                cbr_pid.set_gains(cbr_pid.kp, val, cbr_pid.kd)
                return f"ok ki={val}"
            if key == "kd":
                cbr_pid.set_gains(cbr_pid.kp, cbr_pid.ki, val)
                return f"ok kd={val}"
            if key == "recover-rate":
                recover_rate[0] = val
                return f"ok recover-rate={val}"
            return f"unknown key: {key}"
        return f"unknown command: {line}"

    return handle


def _stop_other_controllers() -> None:
    """Only one local controller should publish to PlotJuggler."""
    me = os.getpid()
    try:
        out = subprocess.check_output(
            ["pgrep", "-f", "scripts/cbr_controller.py"],
            text=True,
            stderr=subprocess.DEVNULL,
        ).strip()
    except subprocess.CalledProcessError:
        return
    for token in out.split():
        pid = int(token)
        if pid != me:
            os.kill(pid, signal.SIGTERM)


def main() -> int:
    a = Args(sys.argv[1:])
    a.default("plot-url", "")
    a.default("host", "127.0.0.1")
    a.default("port", 5090)
    a.default("timeout", 2.5)
    a.default("udp-loss-filter-cutoff", 0.1)
    a.default("udp-loss-steady-fir-length", 128)
    a.default("udp-loss-steady-min-packets", 8)
    a.default("fec-loss-filter-cutoff", 0.1)
    a.default("encode-rate-filter-cutoff", 0.1)
    a.default("cbr-rate-filter-cutoff", 0.1)
    a.default("k", 6)
    a.default("n-gain", 1.2)
    a.default("n-max", 15)
    a.default("kp", 1.0)
    a.default("kd", 0.005)
    a.default("ki", 0.01)
    a.default("recover-rate", 1.1)
    a.default("cbr-min", 100)
    a.default("cbr-max", 20000)
    a.default("loop-rate", 10.0)
    a.default("run-seconds", 0.0)
    a.default("tune-host", "127.0.0.1")
    a.default("tune-port", 5092)

    _stop_other_controllers()

    loop_rate = a("loop-rate")
    dt = 1.0 / loop_rate

    plot = PlotWS(a("plot-url"))
    metric = Metrics(a("host"), a("port"), a("timeout"))
    console = Console(a("host"), a("port"), a("timeout"))
    lpf_udp_loss = LPF(loop_rate, a("udp-loss-filter-cutoff"))
    fir_udp_loss_steady = FirBoxcar(a("udp-loss-steady-fir-length"))
    lpf_fec_loss = LPF(loop_rate, a("fec-loss-filter-cutoff"))
    lpf_encode_rate = LPF(loop_rate, a("encode-rate-filter-cutoff"))
    lpf_cbr = LPF(loop_rate, a("cbr-rate-filter-cutoff"))
    fec_map = FecMap(a("k"), a("n-gain"))
    fec_clamp = Clamp(a("k"), a("n-max"))
    cbr_clamp = Clamp(a("cbr-min"), a("cbr-max"))
    cbr_pid = PID(a("kp"), a("ki"), a("kd"), cbr_clamp, dt, cbr_clamp)
    recover_rate_box = [float(a("recover-rate"))]
    tune = TuneConsole(
        a("tune-host"),
        a("tune-port"),
        _make_tune_handler(cbr_pid, recover_rate_box),
    )
    udp_received_delta = Delay(1)
    fec_received_delta = Delay(1)
    udp_gap_delta = Delay(1)
    fec_gap_delta = Delay(1)
    encode_delta = Delay(1)
    steady_min_pkts = a("udp-loss-steady-min-packets")

    console.line(f"set_fec_k {a('k')}")

    session_gap0: float | None = None
    session_recv0: float | None = None

    t0 = time.monotonic()

    try:
        while True:
            if a("run-seconds") > 0 and time.monotonic() - t0 >= a("run-seconds"):
                break

            tune.poll()

            metric.refresh()
            now_ = time.time()
            udp_received_delta_ = udp_received_delta(
                metric("stream_sender.peer_udp_packet_received")
            )
            fec_received_delta_ = fec_received_delta(
                metric("stream_sender.peer_fec_packet_received")
            )
            udp_gap_delta_ = udp_gap_delta(metric("stream_sender.peer_udp_gap_count"))
            fec_gap_delta_ = fec_gap_delta(metric("stream_sender.peer_fec_gap_count"))
            encode_delta_ = encode_delta(metric("h264_encoder.out_bytes"))
            udp_total_ = udp_gap_delta_ + udp_received_delta_
            fec_total_ = fec_gap_delta_ + fec_received_delta_
            loss_udp_raw_ = udp_gap_delta_ / udp_total_ if udp_total_ else 0.0
            loss_fec_raw_ = fec_gap_delta_ / fec_total_ if fec_total_ else 0.0
            loss_udp_ = lpf_udp_loss(loss_udp_raw_)
            loss_fec_ = lpf_fec_loss(loss_fec_raw_)
            encode_rate_ = lpf_encode_rate(encode_delta_ / dt * 8.0 / 1000.0)

            plot("loss_udp", now_, loss_udp_)
            plot("loss_fec", now_, loss_fec_)
            plot("encode_rate", now_, encode_rate_)

            gap_abs = metric("stream_sender.peer_udp_gap_count")
            recv_abs = metric("stream_sender.peer_udp_packet_received")
            loss_udp_session: float | None = None
            if gap_abs is not None and recv_abs is not None:
                if session_gap0 is None and gap_abs + recv_abs >= steady_min_pkts:
                    session_gap0 = gap_abs
                    session_recv0 = recv_abs
                if session_gap0 is not None and session_recv0 is not None:
                    dg = gap_abs - session_gap0
                    dr = recv_abs - session_recv0
                    session_denom = dg + dr
                    if session_denom >= steady_min_pkts:
                        loss_udp_session = dg / session_denom

            if udp_total_ > 0 and loss_udp_session is not None:
                loss_udp_steady_state_ = fir_udp_loss_steady(loss_udp_session)
            else:
                loss_udp_steady_state_ = fir_udp_loss_steady.value
            plot("loss_udp_steady_state", now_, loss_udp_steady_state_)
            fec_n_preclamp_ = fec_map(loss_udp_steady_state_)
            fec_n_ = fec_clamp(fec_n_preclamp_)
            plot("fec_n", fec_n_)

            if loss_fec_ > 0:
                cbr_setpoint_ = encode_rate_ * (1.0 - loss_fec_)
            else:
                cbr_setpoint_ = encode_rate_ * recover_rate_box[0]
            cbr_error_ = cbr_setpoint_ - cbr_pid.last
            cbr_ = cbr_pid(cbr_error_)
            plot("cbr_error", now_, cbr_error_)
            plot("cbr_setpoint", now_, cbr_setpoint_)
            plot("cbr", now_, cbr_)
            plot("kp", now_, cbr_pid.kp)
            plot("ki", now_, cbr_pid.ki)
            plot("kd", now_, cbr_pid.kd)
            plot("recover_rate", now_, recover_rate_box[0])
            plot.flush()

            console("set_fec_n", fec_n_)
            console("set_encode_cbr", cbr_)

            time.sleep(dt)
    finally:
        tune.close()

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(0)
