"""Configuration constants and defaults."""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path


@dataclass
class Config:
    """Runtime configuration — built from CLI flags + env."""

    # Profiling
    perf_stat_events: list[str] = field(default_factory=lambda: [
        "cache-misses",
        "cache-references",
        "instructions",
        "cycles",
    ])
    perf_record_freq: int = 997   # prime to avoid aliasing
    perf_stat_repeat: int = 3

    # Thresholds
    unresolved_warn_threshold: float = 0.35  # warn if >35% samples unresolved
    top_n_hotspots: int = 5

    # Paths
    output_base: Path = Path("outputs")

    # addr2line
    addr2line_bin: str = "addr2line"

    # Debug
    verbose: bool = False
    keep_raw: bool = True  # always keep perf.data + perf.script.txt
