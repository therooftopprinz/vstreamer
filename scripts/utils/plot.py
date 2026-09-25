from __future__ import annotations

import sys
import time

PLOT_PREFIX = "cbr/"


def _normalize_plot_url(url: str) -> str:
    url = url.strip()
    if not url:
        return ""
    if url.startswith("http://"):
        return "ws://" + url[len("http://") :]
    if url.startswith("https://"):
        return "wss://" + url[len("https://") :]
    return url


class PlotWS:
    def __init__(self, url: str) -> None:
        self._enabled = False
        self._ts: float | None = None
        self._pending: dict[str, float] = {}
        ws_url = _normalize_plot_url(url)
        if not ws_url:
            return
        try:
            from utils.plotjuggler import start_client  # noqa: WPS433

            start_client(ws_url)
            self._enabled = True
        except ImportError:
            print("plot-url disabled: pip install websockets", file=sys.stderr)
        except (RuntimeError, ValueError) as exc:
            print(f"plot-url disabled: {exc}", file=sys.stderr)

    def __call__(self, name: str, *args: float) -> None:
        if not self._enabled:
            return
        key = f"{PLOT_PREFIX}{name}"
        if len(args) >= 2:
            self._ts = args[0]
            self._pending[key] = args[1]
        elif len(args) == 1:
            self._pending[key] = args[0]

    def flush(self) -> None:
        if not self._enabled or not self._pending:
            return
        from utils.plotjuggler import push_tick  # noqa: WPS433

        ts = self._ts if self._ts is not None else time.time()
        push_tick(ts, dict(self._pending))
        self._pending.clear()
        self._ts = None
