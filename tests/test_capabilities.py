"""Tests for capability negotiation decisions."""

from cacheprof.capabilities import _choose_mode
from cacheprof.models import CapabilityReport, CollectionMode


def test_choose_mode_prefers_mem_sampling():
    report = CapabilityReport(mem_sampling_supported=True, available_events=["cache-misses"])
    _choose_mode(report)
    assert report.collection_mode == CollectionMode.MEM_SAMPLING
    assert report.counter_sampling_event is None


def test_choose_mode_picks_best_fallback_event():
    report = CapabilityReport(available_events=["LLC-load-misses"])
    _choose_mode(report)
    assert report.collection_mode == CollectionMode.COUNTER_SAMPLING
    assert report.counter_sampling_event == "LLC-load-misses"
