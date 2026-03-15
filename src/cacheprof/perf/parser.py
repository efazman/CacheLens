"""Parse `perf script` text output into PerfSample objects.

perf script output is notoriously irregular.  This parser is intentionally
lenient — it extracts what it can and drops malformed lines with a counter
so you can tell if something systematic is wrong.

Typical line format (fields depend on -F flags):
  matrix_bad 12345/12345 [001] 1234567.890123:  cache-misses:  0x401234 multiply_bad+0x42 (/home/user/matrix_bad)
"""

from __future__ import annotations

import re
from pathlib import Path

from cacheprof.models import PerfSample
from cacheprof.utils.logging import get_logger

log = get_logger(__name__)

# Pre-compiled patterns for the parts we care about.
# We match greedily because field widths vary across perf versions.

# PID/TID: "12345/12345" or just "12345"
_RE_PIDTID = re.compile(r"(\d+)/(\d+)")

# CPU: "[001]"
_RE_CPU = re.compile(r"\[(\d+)\]")

# Timestamp: "1234567.890123:"
_RE_TIME = re.compile(r"(\d+\.\d+):")

# Event token: "cache-misses:"
_RE_EVENT = re.compile(r"([A-Za-z][\w.-]*):")

# Address: hex, possibly preceded by whitespace
_RE_ADDR = re.compile(r"(0x[0-9a-fA-F]+|[0-9a-fA-F]{6,16})")

# Symbol: "name+0xoffset" or just "name"
_RE_SYM = re.compile(r"([\w.<>:~]+)\+0x[0-9a-fA-F]+")

# DSO: "(path)"
_RE_DSO = re.compile(r"\(([^)]+)\)")


def parse_script_file(path: Path) -> list[PerfSample]:
    """Parse a perf script output file into a list of PerfSample."""
    text = path.read_text(encoding="utf-8", errors="replace")
    return parse_script_text(text)


def parse_script_text(text: str) -> list[PerfSample]:
    """Parse raw perf script text into PerfSample list."""
    samples: list[PerfSample] = []
    skipped = 0

    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue

        sample = _parse_line(line)
        if sample is not None:
            samples.append(sample)
        else:
            skipped += 1

    if skipped:
        log.debug("skipped %d unparseable lines (of %d total)", skipped, skipped + len(samples))

    log.info("parsed %d samples from perf script output", len(samples))
    return samples


def _parse_line(line: str) -> PerfSample | None:
    """Attempt to parse a single perf script line.  Returns None on failure."""
    sample = PerfSample()

    # PID/TID
    m = _RE_PIDTID.search(line)
    if m:
        sample.pid = int(m.group(1))
        sample.tid = int(m.group(2))

    # CPU
    m = _RE_CPU.search(line)
    if m:
        sample.cpu = int(m.group(1))

    # Timestamp is the anchor; some perf versions insert extra fields before
    # the event token, so we search for the last token ending with ":" after it.
    m = _RE_TIME.search(line)
    if not m:
        return None  # can't use a sample without event info
    sample.timestamp = float(m.group(1))

    tail = line[m.end():]
    event_matches = list(_RE_EVENT.finditer(tail))
    if not event_matches:
        return None
    event_match = event_matches[-1]
    sample.event = event_match.group(1)

    # Everything after the event field
    rest = tail[event_match.end():]

    # Address — first hex number in the remainder
    m = _RE_ADDR.search(rest)
    if m:
        addr_str = m.group(1)
        if not addr_str.startswith("0x"):
            addr_str = "0x" + addr_str
        try:
            sample.address = int(addr_str, 16)
        except ValueError:
            pass

    # Symbol
    m = _RE_SYM.search(rest)
    if m:
        sample.symbol = m.group(1)

    # DSO
    m = _RE_DSO.search(rest)
    if m:
        sample.dso = m.group(1)

    return sample
