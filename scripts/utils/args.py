from __future__ import annotations

import argparse
import sys
from typing import Any


class Args:
    """CLI with human-style a.default(key, value) then a(key)."""

    def __init__(self, argv: list[str] | None = None) -> None:
        self._argv = argv if argv is not None else sys.argv[1:]
        self._parser = argparse.ArgumentParser()
        self._dests: dict[str, str] = {}
        self._ns: argparse.Namespace | None = None

    def default(self, key: str, value: Any, **kwargs: Any) -> None:
        dest = key.replace("-", "_")
        self._dests[key] = dest
        self._parser.add_argument(
            f"--{key}",
            dest=dest,
            type=type(value),
            default=value,
            **kwargs,
        )

    def add_argument(self, *args: Any, **kwargs: Any) -> None:
        self._parser.add_argument(*args, **kwargs)

    def _ensure(self) -> argparse.Namespace:
        if self._ns is None:
            self._ns = self._parser.parse_args(self._argv)
        return self._ns

    def __call__(self, key: str) -> Any:
        ns = self._ensure()
        dest = self._dests.get(key, key.replace("-", "_"))
        return getattr(ns, dest)
