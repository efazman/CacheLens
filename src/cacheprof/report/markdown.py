"""Markdown report output — human-readable, saved to run directory."""

from __future__ import annotations

from pathlib import Path

from cacheprof.models import ProfileReport
from cacheprof.utils.logging import get_logger

log = get_logger(__name__)


def write_markdown_report(report: ProfileReport, run_dir: Path) -> Path:
    """Generate a markdown report and write to run_dir/report.md."""
    out_path = run_dir / "report.md"
    lines: list[str] = []

    lines.append("# CacheLens Report")
    lines.append(f"")
    lines.append(f"**Binary:** `{report.target_binary}`  ")
    lines.append(f"**Mode:** {report.collection_mode.name}  ")
    lines.append(f"**Unresolved rate:** {report.unresolved_sample_rate:.1%}")
    lines.append("")

    lines.append("## Capability Check")
    lines.append("")
    lines.append("```text")
    lines.append(
        "\n".join(
            [
                f"perf: {report.capabilities.perf_version or 'not found'}",
                f"addr2line: {report.capabilities.addr2line_version or 'not found'}",
                f"debug info: {report.capabilities.target_has_debug_info}",
                f"mem sampling supported: {report.capabilities.mem_sampling_supported}",
                f"fallback event: {report.capabilities.counter_sampling_event or 'n/a'}",
            ]
        )
    )
    lines.append("```")
    lines.append("")

    if report.capabilities.warnings:
        lines.append("### Warnings")
        lines.append("")
        for warning in report.capabilities.warnings:
            lines.append(f"- {warning}")
        lines.append("")

    if report.capabilities.errors:
        lines.append("### Errors")
        lines.append("")
        for error in report.capabilities.errors:
            lines.append(f"- {error}")
        lines.append("")

    # Counters
    if report.stat_result and report.stat_result.counters:
        lines.append("## Hardware Counters")
        lines.append("")
        lines.append("| Event | Count |")
        lines.append("|-------|------:|")
        for event, count in report.stat_result.counters.items():
            lines.append(f"| {event} | {count:,} |")
        lines.append("")

    # Hotspots
    if report.hotspots:
        lines.append("## Top Hotspots")
        lines.append("")
        lines.append("| # | File:Line | Function | Misses | % Total |")
        lines.append("|---|-----------|----------|-------:|--------:|")
        for i, hs in enumerate(report.hotspots, 1):
            lines.append(
                f"| {i} | `{hs.location.file}:{hs.location.line}` | "
                f"{hs.location.function or '??'} | {hs.miss_count:,} | "
                f"{hs.fraction_of_total:.1%} |"
            )
        lines.append("")

    # Source context for each hotspot
    for i, hs in enumerate(report.hotspots, 1):
        if hs.source_snippet:
            lines.append(f"### Hotspot #{i} — `{hs.location.file}:{hs.location.line}`")
            lines.append("")
            lines.append("```cpp")
            lines.append(hs.source_snippet)
            lines.append("```")
            lines.append("")

    text = "\n".join(lines)
    out_path.write_text(text, encoding="utf-8")
    log.info("Markdown report written to %s", out_path)
    return out_path
