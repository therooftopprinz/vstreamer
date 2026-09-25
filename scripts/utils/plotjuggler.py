"""PlotJuggler JSON WebSocket client (Streaming → WebSocket Server)."""

from __future__ import annotations

import asyncio
import json
import logging
import threading
from typing import Any

import websockets
from websockets.exceptions import ConnectionClosed

logger = logging.getLogger(__name__)

_url = ""
_loop: asyncio.AbstractEventLoop | None = None
_out_queue: asyncio.Queue[str] | None = None
_thread: threading.Thread | None = None
_started = threading.Event()
_stop = threading.Event()


def start_client(url: str) -> None:
    """Connect to PlotJuggler; reconnects in the background."""
    global _url, _thread
    url = url.strip()
    if not url:
        raise ValueError("plotjuggler url must be non-empty")
    if _thread is not None and _thread.is_alive():
        if url == _url:
            return
        raise RuntimeError("plotjuggler client already started with a different url")
    _url = url
    _started.clear()
    _stop.clear()
    _thread = threading.Thread(target=_run_thread, name="plotjuggler", daemon=True)
    _thread.start()
    if not _started.wait(timeout=10.0):
        raise RuntimeError("plotjuggler client failed to start within 10s")


def push_tick(timestamp: float, values: dict[str, float | None]) -> None:
    loop = _loop
    queue = _out_queue
    if loop is None or queue is None or not loop.is_running():
        return
    body: dict[str, Any] = {"timestamp": timestamp}
    for key, val in values.items():
        if val is not None:
            body[key] = val
    if len(body) <= 1:
        return
    payload = json.dumps(body, separators=(",", ":"))
    loop.call_soon_threadsafe(queue.put_nowait, payload)


def _run_thread() -> None:
    asyncio.run(_async_main())


async def _async_main() -> None:
    global _loop, _out_queue
    _loop = asyncio.get_running_loop()
    _out_queue = asyncio.Queue(maxsize=10_000)
    _started.set()

    while not _stop.is_set():
        try:
            async with websockets.connect(
                _url,
                ping_interval=20,
                ping_timeout=20,
                max_queue=32,
            ) as ws:
                logger.info("plotjuggler connected %s", _url)
                while not _stop.is_set():
                    try:
                        payload = await asyncio.wait_for(_out_queue.get(), timeout=0.5)
                    except asyncio.TimeoutError:
                        continue
                    try:
                        await ws.send(payload)
                    except ConnectionClosed:
                        _out_queue.put_nowait(payload)
                        break
        except Exception:
            logger.debug("plotjuggler connect/send failed", exc_info=True)
            await asyncio.sleep(1.0)
