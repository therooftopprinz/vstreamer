from __future__ import annotations

import socket


def udp_console(
    host: str, port: int, line: str, timeout: float = 2.0, recv_bytes: int = 4096
) -> str:
    payload = (line.rstrip("\n") + "\n").encode()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.settimeout(timeout)
        sock.bind(("", 0))
        sock.sendto(payload, (host, port))
        try:
            data, _ = sock.recvfrom(recv_bytes)
        except TimeoutError:
            return ""
        return data.decode(errors="replace").strip()


def metric_value_float(raw: str | None) -> float | None:
    if raw is None:
        return None
    if " <" in raw:
        raw = raw.split(" <", 1)[0]
    token = raw.strip()
    if not token:
        return None
    try:
        return float(token.split()[0])
    except ValueError:
        return None


def parse_metrics_report(text: str) -> dict[str, str]:
    out: dict[str, str] = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or "=" not in line:
            continue
        name, _, value = line.partition("=")
        name = name.strip()
        if name:
            out[name] = value.strip()
    return out


def fetch_pipeline_metrics(host: str, port: int, timeout: float = 2.5) -> str:
    return udp_console(host, port, "metrics", timeout=timeout, recv_bytes=65536)


def console_ok(reply: str) -> bool:
    return bool(reply) and (reply == "ok" or reply.startswith("ok"))


class Metrics:
    def __init__(self, host: str, port: int, timeout: float = 2.5) -> None:
        self._host = host
        self._port = int(port)
        self._timeout = timeout
        self._values: dict[str, str] = {}

    def refresh(self) -> bool:
        reply = fetch_pipeline_metrics(self._host, self._port, self._timeout)
        if not reply or reply.startswith("err"):
            return False
        parsed = parse_metrics_report(reply)
        if not parsed:
            return False
        self._values = parsed
        return True

    def __call__(self, name: str) -> float | None:
        return metric_value_float(self._values.get(name))
