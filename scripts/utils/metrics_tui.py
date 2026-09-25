"""Curses TUI for pipeline UDP metrics snapshots."""

from __future__ import annotations

import argparse
import curses
import json
import os
import re
import sys
import time
from pathlib import Path

from utils.metrics import fetch_pipeline_metrics, parse_metrics_report

MAX_COLUMNS = 6
GUTTER = 1
STATE_VERSION = 1


def view_state_path() -> Path:
    xdg = os.environ.get("XDG_CONFIG_HOME")
    if xdg:
        root = Path(xdg) / "vstreamer"
    else:
        root = Path.home() / ".config" / "vstreamer"
    root.mkdir(parents=True, exist_ok=True)
    return root / "cbr_metrics.json"


def load_view_state() -> dict[str, object]:
    path = view_state_path()
    if not path.is_file():
        return {}
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}
    return raw if isinstance(raw, dict) else {}


def save_view_state(
    *,
    host: str,
    port: int,
    regex: str,
    columns: int,
) -> None:
    path = view_state_path()
    payload = {
        "version": STATE_VERSION,
        "host": host,
        "port": port,
        "regex": regex,
        "columns": max(1, min(MAX_COLUMNS, columns)),
    }
    try:
        path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    except OSError:
        pass


def view_from_state(saved: dict[str, object]) -> tuple[str, int, str, int]:
    host = saved.get("host")
    if not isinstance(host, str) or not host:
        host = "127.0.0.1"
    port_raw = saved.get("port")
    port = int(port_raw) if isinstance(port_raw, int) else 5090
    regex = saved.get("regex")
    if not isinstance(regex, str):
        regex = ""
    cols_raw = saved.get("columns")
    columns = int(cols_raw) if isinstance(cols_raw, int) else 1
    columns = max(1, min(MAX_COLUMNS, columns))
    if regex.strip():
        try:
            re.compile(regex)
        except re.error:
            regex = ""
    return host, port, regex, columns


def match_metrics(
    metrics: dict[str, str], pattern: re.Pattern[str] | None
) -> list[tuple[str, str]]:
    items = list(metrics.items())
    if pattern is None:
        return items
    return [(name, value) for name, value in items if pattern.search(name)]


def truncate(text: str, width: int) -> str:
    if width <= 0:
        return ""
    if len(text) <= width:
        return text
    if width <= 1:
        return text[:width]
    return text[: width - 1] + "…"


def wrap_text(text: str, width: int) -> list[str]:
    if width <= 0:
        return [truncate(text, 0)] if text else [""]
    if not text:
        return [""]
    lines: list[str] = []
    rest = text
    while rest:
        if len(rest) <= width:
            lines.append(rest)
            break
        chunk = rest[:width]
        br = chunk.rfind(" ")
        if br > max(8, width // 4):
            lines.append(rest[:br].rstrip())
            rest = rest[br + 1 :].lstrip()
        else:
            lines.append(chunk)
            rest = rest[width:]
    return lines


def format_metric_lines(name: str, value: str, width: int) -> list[str]:
    sep = " = "
    if width <= 1:
        return [truncate(value, 1)]

    if len(value) <= width:
        need = len(sep) + len(value)
        name_budget = width - need
        if name_budget >= 1:
            show_name = name if len(name) <= name_budget else truncate(name, name_budget)
            left = show_name + sep
            pad = width - len(left) - len(value)
            return [left + (" " * max(0, pad)) + value]

    lines: list[str] = []
    lines.extend(wrap_text(name, width))
    for part in wrap_text(value, width):
        lines.append(part.rjust(width))
    return lines


def column_layout(
    term_width: int, num_columns: int
) -> tuple[list[int], list[int]]:
    num_columns = max(1, min(MAX_COLUMNS, num_columns))
    safe_w = max(1, term_width - 1)
    if num_columns == 1:
        return [0], [safe_w]
    gutters = GUTTER * (num_columns - 1)
    usable = max(num_columns, safe_w - gutters)
    base = usable // num_columns
    rem = usable % num_columns
    widths = [base + (1 if i < rem else 0) for i in range(num_columns)]
    xs = [0]
    for i in range(1, num_columns):
        xs.append(xs[i - 1] + widths[i - 1] + GUTTER)
    return xs, widths


def build_newspaper_columns(
    rows: list[tuple[str, str]], num_columns: int, col_widths: list[int]
) -> list[list[str]]:
    num_columns = max(1, min(MAX_COLUMNS, num_columns))
    columns: list[list[str]] = [[] for _ in range(num_columns)]
    if not rows:
        return columns

    per_col = (len(rows) + num_columns - 1) // num_columns
    for c in range(num_columns):
        w = col_widths[c] if c < len(col_widths) else col_widths[-1]
        start = c * per_col
        end = min(len(rows), start + per_col)
        for name, value in rows[start:end]:
            columns[c].extend(format_metric_lines(name, value, w))
    return columns


def column_scroll_extent(columns: list[list[str]], body_rows: int) -> int:
    if not columns:
        return 0
    tallest = max(len(col) for col in columns)
    return max(0, tallest - body_rows)


def draw_line(stdscr: curses.window, y: int, text: str, attr: int = 0) -> None:
    height, width = stdscr.getmaxyx()
    if y < 0 or y >= height:
        return
    try:
        stdscr.move(y, 0)
        stdscr.clrtoeol()
        stdscr.addnstr(y, 0, text, max(0, width - 1), attr)
    except curses.error:
        pass


def draw_at(
    stdscr: curses.window,
    y: int,
    x: int,
    text: str,
    width: int,
    attr: int = 0,
) -> None:
    height, term_w = stdscr.getmaxyx()
    if y < 0 or y >= height or x >= term_w:
        return
    clip = min(width, term_w - x - 1)
    if clip <= 0:
        return
    if len(text) > clip:
        text = text[:clip]
    try:
        stdscr.addnstr(y, x, text, clip, attr)
    except curses.error:
        pass


def draw_body(
    stdscr: curses.window,
    body_top: int,
    body_rows: int,
    col_xs: list[int],
    col_widths: list[int],
    columns: list[list[str]],
    num_columns: int,
    scroll: int,
) -> None:
    for row in range(body_rows):
        y = body_top + row
        draw_line(stdscr, y, "")
        line_idx = scroll + row
        for c in range(num_columns):
            x = col_xs[c] if c < len(col_xs) else 0
            w = col_widths[c] if c < len(col_widths) else col_widths[-1]
            col_lines = columns[c] if c < len(columns) else []
            text = col_lines[line_idx] if line_idx < len(col_lines) else ""
            draw_at(stdscr, y, x, text, w)


def compile_optional_regex(text: str) -> tuple[re.Pattern[str] | None, str | None]:
    stripped = text.strip()
    if not stripped:
        return None, None
    try:
        return re.compile(stripped), None
    except re.error as exc:
        return None, str(exc)


def run_tui(
    stdscr: curses.window,
    host: str,
    port: int,
    timeout: float,
    rate_hz: float,
    initial_regex: str,
    initial_columns: int,
    view_out: dict[str, object],
) -> None:
    curses.curs_set(0)
    stdscr.nodelay(False)
    stdscr.keypad(True)
    tick_ms = max(50, int(1000.0 / rate_hz)) if rate_hz > 0 else 1000
    stdscr.timeout(tick_ms)

    regex_text = initial_regex
    regex_compiled, startup_err = compile_optional_regex(regex_text)
    if startup_err and regex_text.strip():
        regex_text = ""
        regex_compiled = None

    num_columns = max(1, min(MAX_COLUMNS, initial_columns))
    scroll = 0
    metrics: dict[str, str] = {}
    last_error = ""
    last_ok_mono = 0.0
    editing_regex = False
    input_buf = ""
    input_error = ""

    def refresh_metrics() -> None:
        nonlocal metrics, last_error, last_ok_mono
        reply = fetch_pipeline_metrics(host, port, timeout)
        if not reply or reply.startswith("err"):
            last_error = reply or "timeout"
            return
        parsed = parse_metrics_report(reply)
        if not parsed:
            last_error = "empty or unparseable metrics"
            return
        metrics = parsed
        last_error = ""
        last_ok_mono = time.monotonic()

    refresh_metrics()

    try:
        while True:
            height, width = stdscr.getmaxyx()
            body_top = 3
            body_rows = max(0, height - body_top - 2)
            rows = match_metrics(metrics, regex_compiled)
            col_xs, col_widths = column_layout(width, num_columns)
            columns = build_newspaper_columns(rows, num_columns, col_widths)
            max_scroll = column_scroll_extent(columns, body_rows)
            scroll = max(0, min(scroll, max_scroll))

            status = (
                f"{host}:{port}  metrics={len(metrics)}  shown={len(rows)}"
                f"  cols={num_columns}"
            )
            if last_error:
                status += f"  ERR: {last_error}"
            elif last_ok_mono:
                age = time.monotonic() - last_ok_mono
                status += f"  updated {age:.1f}s ago"
            draw_line(stdscr, 0, truncate(status, width - 1), curses.A_BOLD)

            if editing_regex:
                regex_line = f"regex: {input_buf}_"
                if input_error:
                    regex_line += f"  ({input_error})"
                draw_line(stdscr, 1, truncate(regex_line, width - 1), curses.A_REVERSE)
            elif regex_text:
                draw_line(stdscr, 1, truncate(f"regex: {regex_text}", width - 1))
            else:
                draw_line(stdscr, 1, "regex: (all)")

            if editing_regex:
                draw_line(
                    stdscr,
                    height - 2,
                    "Enter apply  Esc cancel  empty = show all",
                    curses.A_REVERSE,
                )
            else:
                draw_line(
                    stdscr,
                    height - 2,
                    "/ regex  [/] cols  j/k scroll  q quit",
                )

            draw_body(
                stdscr,
                body_top,
                body_rows,
                col_xs,
                col_widths,
                columns,
                num_columns,
                scroll,
            )

            stdscr.refresh()

            ch = stdscr.getch()
            if ch == -1:
                refresh_metrics()
                continue

            if editing_regex:
                if ch in (27,):
                    editing_regex = False
                    input_buf = ""
                    input_error = ""
                    curses.curs_set(0)
                elif ch in (curses.KEY_ENTER, 10, 13):
                    compiled, err = compile_optional_regex(input_buf)
                    if err:
                        input_error = err
                    else:
                        regex_text = input_buf.strip()
                        regex_compiled = compiled
                        scroll = 0
                        editing_regex = False
                        input_buf = ""
                        input_error = ""
                        curses.curs_set(0)
                elif ch in (curses.KEY_BACKSPACE, 127, 8):
                    input_buf = input_buf[:-1]
                    input_error = ""
                elif 32 <= ch <= 126 and len(input_buf) < 240:
                    input_buf += chr(ch)
                    input_error = ""
                continue

            if ch in (ord("q"), ord("Q")):
                break
            if ch in (ord("/"), ord("f"), ord("F")):
                editing_regex = True
                input_buf = regex_text
                input_error = ""
                curses.curs_set(1)
                continue
            if ch in (ord("]"), ord("+")):
                if num_columns < MAX_COLUMNS:
                    num_columns += 1
                    scroll = 0
                continue
            if ch in (ord("["), ord("-")):
                if num_columns > 1:
                    num_columns -= 1
                    scroll = 0
                continue
            if ch in (curses.KEY_UP, ord("k")):
                scroll = max(0, scroll - 1)
            elif ch in (curses.KEY_DOWN, ord("j")):
                scroll = min(max_scroll, scroll + 1)
            elif ch == curses.KEY_PPAGE:
                scroll = max(0, scroll - body_rows)
            elif ch == curses.KEY_NPAGE:
                scroll = min(max_scroll, scroll + body_rows)
            elif ch == curses.KEY_HOME:
                scroll = 0
            elif ch == curses.KEY_END:
                scroll = max_scroll
    finally:
        view_out["host"] = host
        view_out["port"] = port
        view_out["regex"] = regex_text
        view_out["columns"] = num_columns
        curses.curs_set(0)


def main() -> int:
    saved = load_view_state()
    default_host, default_port, default_regex, default_columns = view_from_state(saved)

    parser = argparse.ArgumentParser(description="Pipeline metrics TUI viewer")
    parser.add_argument("--host", default=default_host)
    parser.add_argument("--port", type=int, default=default_port)
    parser.add_argument("--timeout", type=float, default=2.5)
    parser.add_argument(
        "--rate",
        type=float,
        default=5.0,
        help="metrics refresh rate (Hz)",
    )
    parser.add_argument(
        "--columns",
        type=int,
        default=default_columns,
        metavar="N",
        help=f"newspaper columns 1..{MAX_COLUMNS} (wrap overflow within each column)",
    )
    parser.add_argument(
        "--regex",
        default=default_regex,
        metavar="PATTERN",
        help="regex on metric name (empty = show all)",
    )
    args = parser.parse_args()

    if not (1 <= args.columns <= MAX_COLUMNS):
        print(f"--columns must be 1..{MAX_COLUMNS}", file=sys.stderr)
        return 1

    if args.regex.strip():
        _, err = compile_optional_regex(args.regex)
        if err:
            print(f"invalid --regex: {err}", file=sys.stderr)
            return 1

    if not sys.stdin.isatty() or not sys.stdout.isatty():
        print("cbr_metrics.py requires a terminal (TTY)", file=sys.stderr)
        return 1

    view_out: dict[str, object] = {
        "host": args.host,
        "port": args.port,
        "regex": args.regex,
        "columns": args.columns,
    }
    try:
        curses.wrapper(
            run_tui,
            args.host,
            args.port,
            args.timeout,
            args.rate,
            args.regex,
            args.columns,
            view_out,
        )
    except KeyboardInterrupt:
        pass
    finally:
        save_view_state(
            host=str(view_out.get("host", args.host)),
            port=int(view_out.get("port", args.port)),
            regex=str(view_out.get("regex", args.regex)),
            columns=int(view_out.get("columns", args.columns)),
        )
    return 0
