"""Aggregate resolved samples into hotspots by source location.

Groups samples by (file, line), counts misses, and computes the
unresolved sample rate.
"""

from __future__ import annotations

from collections import Counter

from cacheprof.models import Hotspot, ResolvedSample, SourceLocation
from cacheprof.utils.logging import get_logger

log = get_logger(__name__)


def aggregate_hotspots(resolved: list[ResolvedSample]) -> list[Hotspot]:
    """Aggregate resolved samples into a list of Hotspot objects.

    Unresolved samples (file="??") are excluded from hotspots but still
    counted for the unresolved rate computation.

    Returns:
        List of Hotspot sorted by miss_count descending.
    """
    if not resolved:
        return []

    # Count misses per (file, line) key
    counter: Counter[tuple[str, int]] = Counter()
    loc_map: dict[tuple[str, int], SourceLocation] = {}

    for r in resolved:
        key = (r.location.file, r.location.line)
        counter[key] += 1
        # Keep the richest SourceLocation (one with a function name)
        if key not in loc_map or (not loc_map[key].function and r.location.function):
            loc_map[key] = r.location

    total = sum(counter.values())

    hotspots: list[Hotspot] = []
    for (file, line), count in counter.most_common():
        loc = loc_map[(file, line)]
        # Skip unresolved
        if not loc.resolved:
            continue
        hotspots.append(Hotspot(
            location=loc,
            miss_count=count,
            total_samples=total,
            fraction_of_total=count / total if total else 0.0,
        ))

    log.info("aggregated %d hotspots from %d samples", len(hotspots), len(resolved))
    return hotspots


def compute_unresolved_rate(resolved: list[ResolvedSample]) -> float:
    """Fraction of samples where addr2line couldn't resolve to a source line."""
    if not resolved:
        return 0.0
    unresolved = sum(1 for r in resolved if not r.location.resolved)
    return unresolved / len(resolved)
