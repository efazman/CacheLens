"""Terminal (console) report output — plain text, no external dependencies."""

from __future__ import annotations

from cacheprof.capabilities import format_capability_report
from cacheprof.models import ProfileReport


def print_report(report: ProfileReport) -> None:
    """Print a full profile report to the terminal."""
    print()
    print("=" * 60)
    print("  CacheScope Report")
    print("=" * 60)
    print()

    # Capability summary
    print(format_capability_report(report.capabilities))
    print()

    # Perf stat summary
    if report.stat_result and report.stat_result.counters:
        _print_counters(report)

    # Unresolved sample warning
    if report.unresolved_sample_rate > 0.35:
        print(f"⚠  WARNING: {report.unresolved_sample_rate:.0%} of samples "
              f"could not be resolved to source lines.")
        print("   Hints: recompile with -g -O1 or -Og, verify -fno-omit-frame-pointer,")
        print("   ensure you're resolving against the exact binary that was profiled.")
        print()

    # Hotspots table
    if report.hotspots:
        _print_hotspots(report)

    # LLM diagnoses
    for i, (hs, diag) in enumerate(zip(report.hotspots, report.diagnoses)):
        if not diag.summary:
            continue
        print(f"--- Hotspot #{i+1} — {hs.location.file}:{hs.location.line} ---")
        print(f"  {diag.summary}")
        print()
        print(f"  {diag.explanation}")
        print()
        print(f"  Suggestion: {diag.suggestion}")
        print()


def _print_counters(report: ProfileReport) -> None:
    print("Hardware Counters:")
    for event, count in report.stat_result.counters.items():
        print(f"  {event:30s} {count:>15,}")
    if report.stat_result.duration_seconds:
        print(f"  {'duration':30s} {report.stat_result.duration_seconds:.3f}s")
    print()


def _print_hotspots(report: ProfileReport) -> None:
    print("Top Hotspots:")
    print(f"  {'#':>3}  {'File:Line':<40}  {'Function':<25}  {'Misses':>10}  {'% Total':>8}")
    print(f"  {'---':>3}  {'-'*40}  {'-'*25}  {'-'*10}  {'-'*8}")
    for i, hs in enumerate(report.hotspots, 1):
        loc = f"{hs.location.file}:{hs.location.line}"
        print(
            f"  {i:>3}  {loc:<40}  {hs.location.function or '??':<25}  "
            f"{hs.miss_count:>10,}  {hs.fraction_of_total:>7.1%}"
        )
    print()
