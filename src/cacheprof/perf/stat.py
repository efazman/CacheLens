"""Wrapper around `perf stat` — collects aggregate hardware counters.

Usage:
    result = run_perf_stat(binary, args, config)
    print(result.counters)  # {"cache-misses": 3456789, ...}
"""

from __future__ import annotations

import re
from pathlib import Path

from cacheprof.config import Config
from cacheprof.models import PerfStatResult
from cacheprof.utils.logging import get_logger
from cacheprof.utils.subprocess import run

log = get_logger(__name__)

_RE_COUNTER_LINE = re.compile(
    r"^\s*([0-9][0-9,]*(?:\.[0-9]+)?|<[^>]+>)\s+([A-Za-z0-9_.:-]+)\b"
)
_RE_DURATION_LINE = re.compile(r"^\s*([\d.]+)\s+seconds\s+time\s+elapsed\b")


def run_perf_stat(
    binary: str | Path,
    args: list[str],
    config: Config,
    *,
    run_dir: Path | None = None,
) -> PerfStatResult:
    """Execute `perf stat` and parse the counter output.

    Args:
        binary: Path to the target executable.
        args: Arguments to pass to the target.
        config: Runtime configuration (events, repeat count, etc.).
        run_dir: If given, save raw output to run_dir/perf_stat.txt.

    Returns:
        PerfStatResult with parsed counters and duration.
    """
    events = ",".join(config.perf_stat_events)
    cmd = [
        "perf", "stat",
        "-e", events,
        "-r", str(config.perf_stat_repeat),
        "--", str(binary), *args,
    ]

    result = run(cmd, timeout=300)

    # perf stat writes to stderr
    raw = result.stderr

    if run_dir:
        (run_dir / "perf_stat.txt").write_text(raw, encoding="utf-8")

    if not result.ok:
        log.warning("perf stat exited %d: %s", result.returncode, raw[:200])

    return _parse_stat_output(raw)


def _parse_stat_output(raw: str) -> PerfStatResult:
    """Parse perf stat stderr into structured counters.

    Handles formats like:
        3,456,789      cache-misses      #  34.567% of all cache refs  ( +-  2.31% )
        10,000,000     cache-references                                ( +-  0.45% )
        2.345678901 seconds time elapsed
    """
    counters: dict[str, int] = {}
    duration = 0.0

    for line in raw.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            # Skip comment lines but not counter lines that have # for ratios
            if line.startswith("# started"):
                continue

        # Match duration line before generic counters so "seconds time elapsed"
        # does not get misclassified as an event called "seconds".
        m = _RE_DURATION_LINE.match(line)
        if m:
            duration = float(m.group(1))
            continue

        m = _RE_COUNTER_LINE.match(line)
        if m:
            count_str = m.group(1)
            event = m.group(2)
            if count_str.startswith("<"):
                continue
            try:
                counters[event] = int(float(count_str.replace(",", "")))
            except ValueError:
                log.debug("unparseable counter value: %s", count_str)
            continue

    return PerfStatResult(raw_output=raw, counters=counters, duration_seconds=duration)
