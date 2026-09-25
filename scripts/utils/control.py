from __future__ import annotations

import math
from collections import deque
from collections.abc import Callable

ClampFn = Callable[[float], float]


class FirBoxcar:
    """Uniform FIR: sum of last up-to-N samples divided by N (implicit zero pad → ramps from 0)."""

    def __init__(self, length: int) -> None:
        self._n = max(1, int(length))
        self._buf: deque[float] = deque(maxlen=self._n)
        self._sum = 0.0
        self._last_out = 0.0

    @property
    def value(self) -> float:
        return self._last_out

    def __call__(self, x: float) -> float:
        if len(self._buf) == self._n:
            self._sum -= self._buf[0]
        self._buf.append(x)
        self._sum += x
        self._last_out = self._sum / self._n
        return self._last_out


class LPF:
    def __init__(
        self,
        sample_rate: float,
        cutoff: float,
        rolloff: float = 1.0,
        order: int = 1,
        type: str = "lowpass",
    ) -> None:
        del rolloff, order, type
        dt = 1.0 / sample_rate
        rc = 1.0 / (2.0 * math.pi * cutoff)
        self._alpha = dt / (dt + rc)
        self._y = 0.0
        self._ready = False

    def __call__(self, x: float) -> float:
        if not self._ready:
            self._y = x
            self._ready = True
            return x
        self._y += self._alpha * (x - self._y)
        return self._y


class Delay:
    def __init__(self, n: int) -> None:
        self._hist: list[float | None] = [None] * max(1, n)

    def __call__(self, x: float | None) -> float:
        if x is None:
            return 0.0
        oldest = self._hist[-1]
        self._hist = [x] + self._hist[:-1]
        if oldest is None:
            return 0.0
        return x - oldest


class Integrator:
    def __init__(self, clamp_function: ClampFn, dt: float) -> None:
        self._clamp = clamp_function
        self._dt = dt
        self._state = 0.0
        self._ceil: float | None = None

    def max(self, ceiling: float) -> None:
        """Upper limit for integrated CBR (may move down when encode rate drops)."""
        self._ceil = ceiling
        self._state = self._apply_bounds(self._state)

    def _apply_bounds(self, x: float) -> float:
        y = self._clamp(x)
        if self._ceil is not None:
            y = min(y, self._ceil)
        return y

    def __call__(self, rate: float) -> float:
        self._state += rate * self._dt
        self._state = self._apply_bounds(self._state)
        return self._state


class PID:
    def __init__(
        self,
        kp: float,
        ki: float,
        kd: float,
        integral_clamp_function: ClampFn,
        dt: float,
        output_clamp_function: ClampFn | None = None,
    ) -> None:
        self._kp, self._ki, self._kd = kp, ki, kd
        self._integral_clamp = integral_clamp_function
        self._output_clamp = output_clamp_function
        self._dt = dt
        self._integral = 0.0
        self._prev_error: float | None = None
        self._last_out = (
            output_clamp_function(0.0) if output_clamp_function else 0.0
        )

    @property
    def last(self) -> float:
        return self._last_out

    @property
    def kp(self) -> float:
        return self._kp

    @property
    def ki(self) -> float:
        return self._ki

    @property
    def kd(self) -> float:
        return self._kd

    def set_gains(self, kp: float, ki: float, kd: float) -> None:
        self._kp, self._ki, self._kd = kp, ki, kd

    def reset(self) -> None:
        self._integral = 0.0
        self._prev_error = None
        self._last_out = (
            self._output_clamp(0.0) if self._output_clamp else 0.0
        )

    def __call__(self, error: float) -> float:
        self._integral += error * self._dt
        self._integral = self._integral_clamp(self._integral)
        d = 0.0
        if self._prev_error is not None:
            d = (error - self._prev_error) / self._dt
        self._prev_error = error
        out = self._kp * error + self._ki * self._integral + self._kd * d
        if self._output_clamp is not None:
            out = self._output_clamp(out)
        self._last_out = out
        return self._last_out


class FecMap:
    def __init__(self, k: float, gain: float) -> None:
        self._k = k
        self._gain = gain

    def __call__(self, loss_rate: float) -> float:
        p = min(max(loss_rate, 0.0), 0.999999)
        return self._gain * self._k / (1.0 - p)


class Clamp:
    def __init__(self, min: float, max: float) -> None:
        self._min = min
        self._max = max

    def __call__(self, x: float) -> float:
        return max(self._min, min(self._max, x))


def recovery_g(loss: float) -> float:
    gx = [
             1.0, # 0
            -1.0, # 10%
            -2.0, # 20%
            -3.0, # 30%
            -4.0, # 40%
            -5.0, # 50%
            -6.0, # 60%
            -7.0, # 70%
            -8.0, # 80%
            -9.0, # 90%
            -10.0, # 100%
    ]
    # Decile index 0..10: [0%,10%) -> 0, [10%,20%) -> 1, ... 100% -> 10.
    # Do not ceil(loss*100): any epsilon>0 would land in bucket 1 (-1.0).
    L10 = min(10, math.floor(loss * 10.0 + 1e-12))
    return gx[L10]


class RecoveryRate:
    def __init__(self, gain: float) -> None:
        self._gain = gain

    def __call__(self, loss: float) -> float:
        x = min(max(loss, 0.0), 1.0)
        return self._gain * recovery_g(x)
