"""Sample-level data collection via `perf mem` or `perf record`.

Depending on the detected CollectionMode, this module either:
  - MEM_SAMPLING: runs `perf mem record` (precise IP via PEBS/IBS/SPE)
  - COUNTER_SAMPLING: runs `perf record -e cache-misses` (PMU skid)

Both produce a perf.data file that is later processed by perf/script.py.
"""

from __future__ import annotations

from pathlib import Path

from cacheprof.config import Config
from cacheprof.models import CollectionMode
from cacheprof.utils.logging import get_logger
from cacheprof.utils.subprocess import run

log = get_logger(__name__)


def collect_samples(
    binary: str | Path,
    args: list[str],
    config: Config,
    mode: CollectionMode,
    run_dir: Path,
    *,
    counter_event: str | None = None,
) -> Path:
    """Run perf to collect cache-miss samples.  Returns path to perf.data.

    Args:
        binary: Target executable.
        args: Target arguments.
        config: Runtime config.
        mode: MEM_SAMPLING or COUNTER_SAMPLING.
        run_dir: Directory for artifacts.

    Returns:
        Path to the generated perf.data file.

    Raises:
        RuntimeError: If perf exits non-zero.
    """
    perf_data = run_dir / "perf.data"

    if mode == CollectionMode.MEM_SAMPLING:
        cmd = _build_mem_cmd(binary, args, perf_data, config)
    else:
        cmd = _build_record_cmd(binary, args, perf_data, config, counter_event=counter_event)

    log.info("collecting samples: %s", " ".join(cmd))
    result = run(cmd, timeout=600)

    if not result.ok:
        msg = f"perf collection failed (exit {result.returncode}): {result.stderr[:300]}"
        log.error(msg)
        raise RuntimeError(msg)

    if not perf_data.exists():
        raise RuntimeError(f"perf.data not created at {perf_data}")

    size_mb = perf_data.stat().st_size / (1024 * 1024)
    log.info("perf.data: %.1f MB", size_mb)
    return perf_data


def _build_mem_cmd(
    binary: str | Path, args: list[str], perf_data: Path, config: Config
) -> list[str]:
    """Build command for `perf mem record`."""
    return [
        "perf", "mem", "record",
        "-o", str(perf_data),
        "-F", str(config.perf_record_freq),
        "--", str(binary), *args,
    ]


def _build_record_cmd(
    binary: str | Path,
    args: list[str],
    perf_data: Path,
    config: Config,
    *,
    counter_event: str | None = None,
) -> list[str]:
    """Build command for `perf record -e cache-misses` (fallback mode)."""
    event = counter_event or "cache-misses"
    return [
        "perf", "record",
        "-e", event,
        "-o", str(perf_data),
        "-F", str(config.perf_record_freq),
        "--", str(binary), *args,
    ]
