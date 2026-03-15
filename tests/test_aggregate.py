"""Tests for hotspot aggregation."""

from cacheprof.models import PerfSample, SourceLocation, ResolvedSample
from cacheprof.analysis.aggregate import aggregate_hotspots, compute_unresolved_rate


def _make_resolved(file: str, line: int, func: str = "") -> ResolvedSample:
    return ResolvedSample(
        sample=PerfSample(event="cache-misses", address=0x1000),
        location=SourceLocation(file=file, line=line, function=func),
    )


def _make_unresolved() -> ResolvedSample:
    return ResolvedSample(
        sample=PerfSample(event="cache-misses", address=0xDEAD),
        location=SourceLocation(file="??", line=0),
    )


def test_basic_aggregation():
    resolved = [
        _make_resolved("a.cpp", 10, "foo"),
        _make_resolved("a.cpp", 10, "foo"),
        _make_resolved("a.cpp", 10, "foo"),
        _make_resolved("b.cpp", 20, "bar"),
    ]
    hotspots = aggregate_hotspots(resolved)
    assert len(hotspots) == 2
    assert hotspots[0].location.file == "a.cpp"
    assert hotspots[0].miss_count == 3
    assert hotspots[1].miss_count == 1


def test_unresolved_excluded():
    resolved = [
        _make_resolved("a.cpp", 10),
        _make_unresolved(),
        _make_unresolved(),
    ]
    hotspots = aggregate_hotspots(resolved)
    assert len(hotspots) == 1
    assert hotspots[0].location.file == "a.cpp"


def test_unresolved_rate():
    resolved = [
        _make_resolved("a.cpp", 10),
        _make_unresolved(),
        _make_unresolved(),
        _make_unresolved(),
    ]
    rate = compute_unresolved_rate(resolved)
    assert rate == 0.75


def test_empty():
    assert aggregate_hotspots([]) == []
    assert compute_unresolved_rate([]) == 0.0


def test_fraction_of_total():
    resolved = [
        _make_resolved("a.cpp", 10),
        _make_resolved("a.cpp", 10),
        _make_resolved("b.cpp", 20),
        _make_resolved("b.cpp", 20),
    ]
    hotspots = aggregate_hotspots(resolved)
    assert hotspots[0].fraction_of_total == 0.5
    assert hotspots[1].fraction_of_total == 0.5
