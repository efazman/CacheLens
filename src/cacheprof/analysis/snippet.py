"""Extract source code snippets around hotspot lines.

Reads the actual source file and grabs a window of lines centered
on the hotspot.  If the file can't be read, the snippet is left empty.
"""

from __future__ import annotations

from pathlib import Path

from cacheprof.models import Hotspot
from cacheprof.utils.logging import get_logger

log = get_logger(__name__)

DEFAULT_CONTEXT = 8  # lines above and below the hotspot line


def attach_snippets(
    hotspots: list[Hotspot],
    *,
    search_roots: list[Path] | None = None,
    context_lines: int = DEFAULT_CONTEXT,
) -> None:
    """Attach source_snippet to each Hotspot (mutates in place).

    If the source file can't be found or read, source_snippet stays None.
    """
    # Cache file contents to avoid re-reading the same file
    cache: dict[str, list[str] | None] = {}

    for hs in hotspots:
        loc = hs.location
        if not loc.resolved:
            continue

        if loc.file not in cache:
            cache[loc.file] = _read_file(loc.file, search_roots or [])

        lines = cache[loc.file]
        if lines is None:
            continue

        start = max(0, loc.line - 1 - context_lines)
        end = min(len(lines), loc.line + context_lines)
        numbered = [
            f"{i + 1:>5}{'→' if i + 1 == loc.line else ' '} {lines[i]}"
            for i in range(start, end)
        ]
        hs.source_snippet = "\n".join(numbered)


def _read_file(path: str, search_roots: list[Path]) -> list[str] | None:
    """Read a source file, returning lines or None on failure."""
    candidates: list[Path] = []
    src = Path(path)
    if src.is_absolute():
        candidates.append(src)
    else:
        candidates.append(src)
        for root in search_roots:
            candidates.append(root / src)

    seen: set[Path] = set()
    for candidate in candidates:
        normalized = candidate.resolve(strict=False)
        if normalized in seen:
            continue
        seen.add(normalized)
        try:
            return normalized.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            continue

    log.debug("can't read source file %s", path)
    return None
