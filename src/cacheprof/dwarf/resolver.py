"""Resolve instruction addresses to source locations via addr2line.

Uses a single addr2line subprocess with addresses piped through stdin for
performance. We intentionally avoid inline frame expansion in v1 so the
output stays deterministic: exactly two lines per address.

addr2line returns "??:0" for unresolved addresses — we handle this as a
first-class case, not an error.
"""

from __future__ import annotations

import subprocess
from pathlib import Path

from cacheprof.config import Config
from cacheprof.models import PerfSample, ResolvedSample, SourceLocation
from cacheprof.utils.logging import get_logger

log = get_logger(__name__)


def resolve_samples(
    samples: list[PerfSample],
    binary: str | Path,
    config: Config,
) -> list[ResolvedSample]:
    """Resolve all sample addresses to source locations in batch.

    Args:
        samples: Parsed perf samples with .address fields.
        binary: The binary that was profiled (must have DWARF info).
        config: Runtime config (for addr2line binary path).

    Returns:
        List of ResolvedSample (same length as input; unresolved samples
        get a SourceLocation with file="??" and line=0).
    """
    if not samples:
        return []

    # Deduplicate addresses to minimize addr2line calls
    unique_addrs = sorted({s.address for s in samples if s.address})
    log.info("resolving %d unique addresses (from %d samples)", len(unique_addrs), len(samples))

    addr_map = _batch_resolve(unique_addrs, str(binary), config.addr2line_bin)

    # Build resolved samples
    resolved: list[ResolvedSample] = []
    for sample in samples:
        loc = addr_map.get(sample.address, _unresolved())
        resolved.append(ResolvedSample(sample=sample, location=loc))

    n_resolved = sum(1 for r in resolved if r.location.resolved)
    log.info(
        "resolution: %d/%d resolved (%.1f%% unresolved)",
        n_resolved,
        len(resolved),
        100 * (1 - n_resolved / len(resolved)) if resolved else 0,
    )
    return resolved


def _batch_resolve(
    addresses: list[int],
    binary: str,
    addr2line_bin: str,
) -> dict[int, SourceLocation]:
    """Pipe addresses through a single addr2line process.

    addr2line with -f returns pairs of lines per address:
        function_name
        /path/to/file.cpp:42
    """
    if not addresses:
        return {}

    input_text = "\n".join(f"0x{addr:x}" for addr in addresses) + "\n"

    try:
        proc = subprocess.run(
            [addr2line_bin, "-f", "-C", "-e", binary],
            input=input_text,
            capture_output=True,
            text=True,
            timeout=60,
        )
    except FileNotFoundError:
        log.error("%s not found", addr2line_bin)
        return {}
    except subprocess.TimeoutExpired:
        log.error("addr2line timed out after 60s")
        return {}

    if proc.returncode != 0:
        log.warning("addr2line exited %d: %s", proc.returncode, proc.stderr[:200])

    return _parse_addr2line_output(addresses, proc.stdout)


def _parse_addr2line_output(
    addresses: list[int], output: str
) -> dict[int, SourceLocation]:
    """Parse addr2line output into a mapping of address → SourceLocation.

    With -f, output comes in pairs:
        function_name
        file:line
    """
    lines = output.splitlines()
    result: dict[int, SourceLocation] = {}

    # Without -i, addr2line emits exactly two lines per address.
    idx = 0
    for addr in addresses:
        if idx + 1 < len(lines):
            func = lines[idx].strip()
            file_line = lines[idx + 1].strip()
            idx += 2
            result[addr] = _parse_file_line(file_line, func)
        else:
            result[addr] = _unresolved()

    return result


def _parse_file_line(file_line: str, function: str) -> SourceLocation:
    """Parse 'file:line' string from addr2line."""
    # Handle "??:0", "??:?", discriminator formats like "file:42 (discriminator 1)"
    is_inlined = False

    # Strip discriminator suffix
    if " (discriminator" in file_line:
        file_line = file_line.split(" (discriminator")[0].strip()

    parts = file_line.rsplit(":", 1)
    if len(parts) == 2:
        file_path = parts[0]
        try:
            line_no = int(parts[1])
        except ValueError:
            line_no = 0
    else:
        file_path = file_line
        line_no = 0

    return SourceLocation(
        file=file_path,
        line=line_no,
        function=function if function != "??" else "",
        is_inlined=is_inlined,
    )


def _unresolved() -> SourceLocation:
    return SourceLocation(file="??", line=0, function="")
