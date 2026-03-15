"""Pre-flight capability check.

Runs before any profiling to detect what the current system supports,
choose a collection mode, and surface actionable warnings early.

Usage:
    from cacheprof.capabilities import check_capabilities
    report = check_capabilities(binary_path, config)
    if not report.can_proceed:
        # bail out with report.errors
"""

from __future__ import annotations

import shutil
from pathlib import Path

from cacheprof.config import Config
from cacheprof.models import CapabilityReport, CollectionMode
from cacheprof.utils.logging import get_logger
from cacheprof.utils.subprocess import run

log = get_logger(__name__)


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def check_capabilities(binary: str | Path, config: Config) -> CapabilityReport:
    """Run all capability probes and return a CapabilityReport.

    This is the single entry-point.  Every probe is best-effort: a failing
    probe adds to warnings/errors but never raises.
    """
    report = CapabilityReport()

    _check_perf(report)
    _check_addr2line(report, config)
    _probe_events(report)
    _probe_mem_sampling(report)
    _check_debug_info(report, Path(binary))
    _choose_mode(report)

    log.info(
        "capabilities: mode=%s  perf=%s  addr2line=%s  debug_info=%s",
        report.collection_mode.name,
        report.perf_version or "missing",
        report.addr2line_version or "missing",
        report.target_has_debug_info,
    )
    return report


# ---------------------------------------------------------------------------
# Individual probes
# ---------------------------------------------------------------------------

def _check_perf(report: CapabilityReport) -> None:
    """Verify perf is installed and capture its version string."""
    if not shutil.which("perf"):
        report.errors.append("perf not found in $PATH — install linux-tools-common")
        return

    result = run(["perf", "version"], timeout=5)
    if result.ok:
        report.perf_available = True
        report.perf_version = result.stdout.strip()
    else:
        report.errors.append(f"perf version failed: {result.stderr.strip()}")


def _check_addr2line(report: CapabilityReport, config: Config) -> None:
    """Verify addr2line is available."""
    bin_name = config.addr2line_bin
    if not shutil.which(bin_name):
        report.errors.append(
            f"{bin_name} not found in $PATH — install binutils"
        )
        return

    result = run([bin_name, "--version"], timeout=5)
    if result.ok:
        report.addr2line_available = True
        # first line is usually "GNU addr2line (GNU Binutils ...) X.Y.Z"
        first_line = result.stdout.split("\n", 1)[0].strip()
        report.addr2line_version = first_line
    else:
        report.warnings.append(f"{bin_name} --version returned non-zero")
        # Still mark available — some builds exit 1 but work fine
        report.addr2line_available = True


def _probe_events(report: CapabilityReport) -> None:
    """Use `perf list` to discover which HW cache events are available.

    We do NOT assume any particular event name exists — this is the whole
    point of probing.  LLC-load-misses, for instance, is not universal.
    """
    if not report.perf_available:
        return

    result = run(["perf", "list"], timeout=10)
    if not result.ok:
        report.warnings.append("perf list failed — event discovery skipped")
        return

    candidates = ("cache-misses", "cache-references", "LLC-load-misses", "LLC-loads")
    stdout = result.stdout
    events = [event for event in candidates if event in stdout]
    report.available_events = events
    log.debug("discovered cache-related events: %s", events)


def _probe_mem_sampling(report: CapabilityReport) -> None:
    """Attempt a quick dry-run of `perf mem` to see if memory sampling works.

    perf mem relies on architecture-specific facilities:
      Intel → PEBS,  AMD → IBS Op,  Arm64 → SPE.
    If the kernel or hardware doesn't support it, perf mem will error out.
    """
    if not report.perf_available:
        return

    # We use `perf mem record -e list` or a short dry-run against /bin/true
    # to test without actually profiling anything meaningful.
    result = run(
        ["perf", "mem", "record", "-o", "/dev/null", "--", "/bin/true"],
        timeout=10,
    )

    if result.ok:
        report.mem_sampling_supported = True
        log.info("perf mem record: supported")
    else:
        report.mem_sampling_supported = False
        snippet = result.stderr.strip()[:200]
        report.warnings.append(
            f"perf mem not available (falling back to counter sampling): {snippet}"
        )
        log.info("perf mem record: NOT supported — %s", snippet)


def _check_debug_info(report: CapabilityReport, binary: Path) -> None:
    """Check whether the target binary has DWARF debug info.

    Uses `file` as a hint and `readelf -S` to confirm the presence of a
    .debug_info section. "not stripped" alone is not enough: a binary can
    have symbols but no DWARF line info.
    """
    if not binary.exists():
        report.errors.append(f"target binary not found: {binary}")
        return

    # Quick check with `file`
    result = run(["file", str(binary)], timeout=5)
    if result.ok:
        out = result.stdout.lower()
        if "with debug_info" in out:
            report.target_has_debug_info = True
            return

    # Fallback: look for .debug_info section header
    if shutil.which("readelf"):
        result = run(["readelf", "-S", str(binary)], timeout=5)
        if result.ok and ".debug_info" in result.stdout:
            report.target_has_debug_info = True
            return

    report.errors.append(
        f"No DWARF debug info detected in {binary.name} — "
        "addr2line will return ??:0 for every address.  Recompile with -g."
    )


def _choose_mode(report: CapabilityReport) -> None:
    """Select the best collection mode based on probe results."""
    if report.mem_sampling_supported:
        report.collection_mode = CollectionMode.MEM_SAMPLING
        report.counter_sampling_event = None
    else:
        preferred = ["cache-misses", "LLC-load-misses", "LLC-loads"]
        found = next((event for event in preferred if event in report.available_events), None)
        if found:
            report.collection_mode = CollectionMode.COUNTER_SAMPLING
            report.counter_sampling_event = found
        else:
            report.errors.append(
                "No usable cache event found (tried: cache-misses, "
                "LLC-load-misses, LLC-loads).  Check perf list output."
            )
            report.collection_mode = CollectionMode.COUNTER_SAMPLING
            report.counter_sampling_event = None


# ---------------------------------------------------------------------------
# Pretty-print for terminal / report embedding
# ---------------------------------------------------------------------------

def format_capability_report(report: CapabilityReport) -> str:
    """Human-readable summary of a CapabilityReport."""
    lines: list[str] = ["=== Capability Check ==="]

    def status(ok: bool) -> str:
        return "✓" if ok else "✗"

    lines.append(f"  perf           {status(report.perf_available)}  {report.perf_version or 'not found'}")
    lines.append(f"  addr2line      {status(report.addr2line_available)}  {report.addr2line_version or 'not found'}")
    lines.append(f"  debug info     {status(report.target_has_debug_info)}")
    lines.append(f"  mem sampling   {status(report.mem_sampling_supported)}")
    lines.append(f"  mode chosen    {report.collection_mode.name}")
    if report.counter_sampling_event:
        lines.append(f"  fallback event {report.counter_sampling_event}")

    if report.available_events:
        lines.append(f"  cache events   {', '.join(report.available_events[:8])}")

    for w in report.warnings:
        lines.append(f"  ⚠  {w}")
    for e in report.errors:
        lines.append(f"  ✗  {e}")

    if report.can_proceed:
        lines.append("  → Ready to profile.")
    else:
        lines.append("  → Cannot proceed — fix errors above.")

    return "\n".join(lines)
