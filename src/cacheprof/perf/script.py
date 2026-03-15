"""Convert perf.data into text via `perf script`.

The text output is the basis for all downstream parsing — it gives us
one line per sample with PID, timestamp, event, address, and symbol.
"""

from __future__ import annotations

from pathlib import Path

from cacheprof.utils.logging import get_logger
from cacheprof.utils.subprocess import run

log = get_logger(__name__)


def run_perf_script(perf_data: Path, run_dir: Path) -> Path:
    """Run `perf script` on perf_data and write output to run_dir/perf_script.txt.

    Returns:
        Path to the generated text file.

    Raises:
        RuntimeError: If perf script fails.
    """
    output_path = run_dir / "perf_script.txt"

    cmd = [
        "perf", "script",
        "-i", str(perf_data),
        "-F", "pid,tid,cpu,time,event,ip,sym,dso",
    ]

    log.info("running perf script")
    result = run(cmd, timeout=120)

    if not result.ok:
        msg = f"perf script failed (exit {result.returncode}): {result.stderr[:300]}"
        log.error(msg)
        # Save whatever we got — partial failure model
        if result.stdout:
            output_path.write_text(result.stdout, encoding="utf-8")
        raise RuntimeError(msg)

    output_path.write_text(result.stdout, encoding="utf-8")
    line_count = result.stdout.count("\n")
    log.info("perf script: %d lines → %s", line_count, output_path)
    return output_path
