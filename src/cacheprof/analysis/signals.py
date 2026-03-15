"""Signal extraction for hotspots.

This module outputs STRUCTURED QUANTITATIVE SIGNALS, not diagnoses.
The LLM does the reasoning.  We just give it data to reason about.
"""

from __future__ import annotations

import re
from pathlib import Path

from cacheprof.models import Hotspot, HotspotSignals, PerfStatResult
from cacheprof.utils.logging import get_logger

log = get_logger(__name__)


def extract_signals(
    hotspots: list[Hotspot],
    stat_result: PerfStatResult | None,
    unresolved_rate: float,
) -> list[HotspotSignals]:
    """Extract quantitative signals for each hotspot.

    Args:
        hotspots: Ranked hotspots with source snippets attached.
        stat_result: Aggregate perf stat counters (for LLC miss rate).
        unresolved_rate: Fraction of samples that couldn't be resolved.

    Returns:
        Parallel list of HotspotSignals (same order as hotspots).
    """
    llc_miss_rate = _compute_llc_miss_rate(stat_result) if stat_result else 0.0

    signals: list[HotspotSignals] = []
    for hs in hotspots:
        snippet = hs.source_snippet or ""
        signals.append(HotspotSignals(
            llc_miss_rate=llc_miss_rate,
            hotspot_concentration=hs.fraction_of_total,
            has_nested_loops=_detect_nested_loops(snippet),
            has_pointer_deref=_detect_pointer_deref(snippet),
            dataset_size_hint=_guess_dataset_size(snippet),
            unresolved_sample_rate=unresolved_rate,
        ))

    return signals


# ---------------------------------------------------------------------------
# Individual signal computations
# ---------------------------------------------------------------------------

def _compute_llc_miss_rate(stat: PerfStatResult) -> float:
    """LLC miss rate = cache-misses / cache-references."""
    misses = sum(
        count for event, count in stat.counters.items()
        if event == "cache-misses" or event.startswith("cache-misses:")
    )
    refs = sum(
        count for event, count in stat.counters.items()
        if event == "cache-references" or event.startswith("cache-references:")
    )
    if refs == 0:
        return 0.0
    return misses / refs


def _detect_nested_loops(snippet: str) -> bool:
    """Heuristic: does the snippet contain nested for/while loops?"""
    # Count loop keywords at different indentation levels
    loop_kw = re.findall(r"\b(for|while)\b", snippet)
    return len(loop_kw) >= 2


def _detect_pointer_deref(snippet: str) -> bool:
    """Heuristic: does the snippet contain pointer dereferences?"""
    # Matches: ptr->field, *ptr, arr[expr] where expr contains a variable
    patterns = [
        r"\w+->\w+",           # ptr->field
        r"\*\w+",              # *ptr
        r"\w+\[(?![\d\]]+\])", # arr[non-constant-index]
    ]
    for p in patterns:
        if re.search(p, snippet):
            return True
    return False


def _guess_dataset_size(snippet: str) -> int | None:
    """Heuristic: try to extract a size constant from the snippet.

    Looks for patterns like N = 1024, SIZE = 4096, constexpr int N = 512, etc.
    Returns None if nothing is found — don't invent data.
    """
    m = re.search(r"\b(?:N|SIZE|LEN|COUNT|NUM)\s*=\s*(\d+)", snippet, re.IGNORECASE)
    if m:
        return int(m.group(1))
    return None
