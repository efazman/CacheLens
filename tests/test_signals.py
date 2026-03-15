"""Tests for signal extraction helpers."""

from cacheprof.analysis.signals import _compute_llc_miss_rate
from cacheprof.models import PerfStatResult


def test_compute_llc_miss_rate_with_qualified_events():
    stat = PerfStatResult(
        raw_output="",
        counters={
            "cache-misses:u": 300,
            "cache-references:u": 1200,
        },
    )
    assert _compute_llc_miss_rate(stat) == 0.25
