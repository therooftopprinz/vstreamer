from __future__ import annotations

from utils.metrics import console_ok, udp_console


class Console:
    def __init__(self, host: str, port: int, timeout: float = 2.5) -> None:
        self._host = host
        self._port = int(port)
        self._timeout = timeout

    def __call__(self, command: str, value: float | int) -> bool:
        if command == "set_fec_n":
            line = f"set_fec_n {int(round(value))}"
        elif command == "set_encode_cbr":
            line = f"set_encode_cbr {int(round(value))}"
        else:
            line = f"{command} {value}"
        reply = udp_console(self._host, self._port, line, timeout=self._timeout)
        return console_ok(reply)

    def line(self, text: str) -> bool:
        reply = udp_console(self._host, self._port, text, timeout=self._timeout)
        return console_ok(reply)
