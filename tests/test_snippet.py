"""Tests for source snippet attachment."""

from pathlib import Path

from cacheprof.analysis.snippet import attach_snippets
from cacheprof.models import Hotspot, SourceLocation


def test_attach_snippets_uses_search_roots(tmp_path: Path):
    src_dir = tmp_path / "benchmarks"
    src_dir.mkdir()
    source = src_dir / "matrix_bad.cpp"
    source.write_text("line1\nline2\nline3\n", encoding="utf-8")

    hotspot = Hotspot(location=SourceLocation(file="matrix_bad.cpp", line=2, function="f"))
    attach_snippets([hotspot], search_roots=[src_dir], context_lines=0)

    assert hotspot.source_snippet is not None
    assert "line2" in hotspot.source_snippet
    assert "→" in hotspot.source_snippet
