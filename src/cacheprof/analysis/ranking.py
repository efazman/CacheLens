"""Rank and filter hotspots for reporting and LLM analysis."""

from __future__ import annotations

from cacheprof.models import Hotspot
from cacheprof.utils.logging import get_logger

log = get_logger(__name__)


def top_hotspots(hotspots: list[Hotspot], n: int = 5) -> list[Hotspot]:
    """Return the top *n* hotspots by miss_count.

    Input should already be sorted (aggregate.py returns sorted), but
    we re-sort defensively.
    """
    ranked = sorted(hotspots, key=lambda h: h.miss_count, reverse=True)[:n]
    log.info("top %d hotspots selected (of %d)", len(ranked), len(hotspots))
    return ranked
