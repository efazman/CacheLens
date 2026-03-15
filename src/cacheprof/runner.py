"""Pipeline runner — orchestrates the full profiling flow.

Implements the partial failure model: every completed stage's artifacts
are saved even if a later stage fails.  Never silently discard work.
"""

from __future__ import annotations

from pathlib import Path

from cacheprof.capabilities import check_capabilities
from cacheprof.config import Config
from cacheprof.models import ProfileReport
from cacheprof.perf.stat import run_perf_stat
from cacheprof.perf.mem import collect_samples
from cacheprof.perf.script import run_perf_script
from cacheprof.perf.parser import parse_script_file
from cacheprof.dwarf.resolver import resolve_samples
from cacheprof.analysis.aggregate import aggregate_hotspots, compute_unresolved_rate
from cacheprof.analysis.ranking import top_hotspots
from cacheprof.analysis.snippet import attach_snippets
from cacheprof.analysis.signals import extract_signals
from cacheprof.report.json_report import write_json_report
from cacheprof.report.markdown import write_markdown_report
from cacheprof.utils.fs import create_run_dir, save_artifact
from cacheprof.utils.logging import get_logger

log = get_logger(__name__)


def run_pipeline(
    binary: str,
    args: list[str],
    config: Config,
) -> ProfileReport:
    """Execute the full profiling pipeline.

    Returns a ProfileReport with whatever stages completed successfully.
    """
    report = ProfileReport(target_binary=binary, target_args=args)
    run_dir = create_run_dir(config.output_base)
    report.run_dir = run_dir
    log.info("run directory: %s", run_dir)

    # --- Stage 1: Capability check ---
    cap = check_capabilities(binary, config)
    report.capabilities = cap
    report.collection_mode = cap.collection_mode
    save_artifact(run_dir, "capabilities.txt", _cap_summary(cap))

    if not cap.can_proceed:
        log.error("capability check failed — aborting")
        _save_partial(report, run_dir)
        return report

    # --- Stage 2: perf stat (aggregate counters) ---
    try:
        stat_result = run_perf_stat(binary, args, config, run_dir=run_dir)
        report.stat_result = stat_result
    except Exception as exc:
        log.error("perf stat failed: %s", exc)
        _save_partial(report, run_dir)
        return report

    # --- Stage 3: Sample collection ---
    try:
        perf_data = collect_samples(
            binary,
            args,
            config,
            cap.collection_mode,
            run_dir,
            counter_event=cap.counter_sampling_event,
        )
    except Exception as exc:
        log.error("sample collection failed: %s", exc)
        _save_partial(report, run_dir)
        return report

    # --- Stage 4: perf script ---
    try:
        script_path = run_perf_script(perf_data, run_dir)
    except Exception as exc:
        log.error("perf script failed: %s", exc)
        _save_partial(report, run_dir)
        return report

    # --- Stage 5: Parse samples ---
    samples = parse_script_file(script_path)
    if not samples:
        log.warning("no samples parsed — the binary may have run too briefly")
        _save_partial(report, run_dir)
        return report

    # --- Stage 6: Address resolution ---
    resolved = resolve_samples(samples, binary, config)

    # --- Stage 7: Aggregation + ranking ---
    report.unresolved_sample_rate = compute_unresolved_rate(resolved)
    all_hotspots = aggregate_hotspots(resolved)
    report.hotspots = top_hotspots(all_hotspots, config.top_n_hotspots)

    # --- Stage 8: Snippet extraction ---
    attach_snippets(
        report.hotspots,
        search_roots=[Path.cwd(), Path(binary).resolve().parent],
    )

    # --- Stage 9: Signal extraction ---
    report.signals = extract_signals(
        report.hotspots, report.stat_result, report.unresolved_sample_rate
    )

    # --- Stage 10: Optional LLM explanations ---
    if config.enable_llm and report.hotspots and report.signals:
        try:
            from cacheprof.llm.explain import explain_hotspots

            report.diagnoses = explain_hotspots(
                report.hotspots,
                report.signals,
                report.collection_mode,
                config,
            )
        except Exception as exc:
            log.warning("LLM stage failed: %s", exc)

    # --- Stage 11: Reports ---
    write_json_report(report, run_dir)
    write_markdown_report(report, run_dir)

    log.info("pipeline complete — results in %s", run_dir)
    return report


def _save_partial(report: ProfileReport, run_dir: Path) -> None:
    """Save whatever we have so far — partial failure model."""
    try:
        write_json_report(report, run_dir)
    except Exception as exc:
        log.warning("couldn't save partial JSON report: %s", exc)


def _cap_summary(cap) -> str:
    from cacheprof.capabilities import format_capability_report
    return format_capability_report(cap)
