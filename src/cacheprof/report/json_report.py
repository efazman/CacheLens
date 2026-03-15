"""JSON report output — machine-readable, saved to the run directory."""

from __future__ import annotations

import json
from dataclasses import asdict
from pathlib import Path

from cacheprof.models import ProfileReport
from cacheprof.utils.logging import get_logger

log = get_logger(__name__)


def write_json_report(report: ProfileReport, run_dir: Path) -> Path:
    """Serialize the report to JSON and write to run_dir/report.json."""
    out_path = run_dir / "report.json"

    data = _serialize(report)
    text = json.dumps(data, indent=2, default=str)
    out_path.write_text(text, encoding="utf-8")

    log.info("JSON report written to %s", out_path)
    return out_path


def _serialize(report: ProfileReport) -> dict:
    """Convert ProfileReport to a JSON-safe dict."""
    return {
        "capabilities": {
            "perf_available": report.capabilities.perf_available,
            "perf_version": report.capabilities.perf_version,
            "addr2line_available": report.capabilities.addr2line_available,
            "addr2line_version": report.capabilities.addr2line_version,
            "target_has_debug_info": report.capabilities.target_has_debug_info,
            "mem_sampling_supported": report.capabilities.mem_sampling_supported,
            "available_events": report.capabilities.available_events,
            "collection_mode": report.capabilities.collection_mode.name,
            "counter_sampling_event": report.capabilities.counter_sampling_event,
            "warnings": report.capabilities.warnings,
            "errors": report.capabilities.errors,
            "can_proceed": report.capabilities.can_proceed,
        },
        "collection_mode": report.collection_mode.name,
        "target_binary": report.target_binary,
        "target_args": report.target_args,
        "run_dir": str(report.run_dir) if report.run_dir else None,
        "unresolved_sample_rate": report.unresolved_sample_rate,
        "counters": report.stat_result.counters if report.stat_result else {},
        "duration_seconds": report.stat_result.duration_seconds if report.stat_result else 0,
        "hotspots": [
            {
                "rank": i + 1,
                "file": hs.location.file,
                "line": hs.location.line,
                "function": hs.location.function,
                "miss_count": hs.miss_count,
                "fraction_of_total": round(hs.fraction_of_total, 4),
                "snippet": hs.source_snippet,
            }
            for i, hs in enumerate(report.hotspots)
        ],
        "signals": [asdict(s) for s in report.signals],
        "diagnoses": [
            {
                "summary": d.summary,
                "explanation": d.explanation,
                "suggestion": d.suggestion,
                "raw_response": d.raw_response,
                "parse_ok": d.parse_ok,
            }
            for d in report.diagnoses
        ],
    }
