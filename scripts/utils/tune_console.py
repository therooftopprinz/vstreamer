from __future__ import annotations

import queue
import socket
import threading
from collections.abc import Callable
from typing import Any

HandleFn = Callable[[str], str]


class TuneConsole:
    """UDP line console for runtime parameter injection (127.0.0.1 by default)."""

    def __init__(self, host: str, port: int, handle: HandleFn) -> None:
        self._handle = handle
        self._queue: queue.Queue[tuple[str, Any]] = queue.Queue()
        self._sock: socket.socket | None = None
        self._thread: threading.Thread | None = None
        self._stop = threading.Event()
        if port <= 0:
            return
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._sock.bind((host, int(port)))
        self._sock.settimeout(0.25)
        self._thread = threading.Thread(target=self._recv_loop, daemon=True)
        self._thread.start()

    def _recv_loop(self) -> None:
        assert self._sock is not None
        while not self._stop.is_set():
            try:
                data, addr = self._sock.recvfrom(4096)
            except socket.timeout:
                continue
            except OSError:
                break
            try:
                line = data.decode("utf-8", errors="replace").strip()
            except Exception:
                line = ""
            if line:
                self._queue.put((line, addr))

    def poll(self) -> None:
        if self._sock is None:
            return
        while True:
            try:
                line, addr = self._queue.get_nowait()
            except queue.Empty:
                break
            reply = self._handle(line)
            if not reply.endswith("\n"):
                reply += "\n"
            try:
                self._sock.sendto(reply.encode("utf-8"), addr)
            except OSError:
                pass

    def close(self) -> None:
        self._stop.set()
        if self._sock is not None:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None
