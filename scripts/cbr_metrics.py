#!/usr/bin/env python3
"""TUI viewer for pipeline metrics (UDP console `metrics` snapshot).

  python3 scripts/cbr_metrics.py
  python3 scripts/cbr_metrics.py --regex 'stream_sender\\.loss' --columns 3

Keys: / edit regex  j/k scroll  [/] columns  q quit  (empty regex = show all)

Restores last regex, columns, host, and port from ~/.config/vstreamer/cbr_metrics.json on start; saves on exit.
"""

from __future__ import annotations

import sys
from pathlib import Path

_SCRIPTS = Path(__file__).resolve().parent
if str(_SCRIPTS) not in sys.path:
    sys.path.insert(0, str(_SCRIPTS))

from utils.metrics_tui import main  # noqa: E402

if __name__ == "__main__":
    raise SystemExit(main())
