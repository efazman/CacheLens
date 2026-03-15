"""Tests for the addr2line resolver."""

from cacheprof.dwarf.resolver import (
    _parse_addr2line_output,
    _parse_file_line,
    _unresolved,
)


def test_parse_normal():
    loc = _parse_file_line("/src/main.cpp:42", "do_work")
    assert loc.file == "/src/main.cpp"
    assert loc.line == 42
    assert loc.function == "do_work"
    assert loc.resolved


def test_parse_unresolved():
    loc = _parse_file_line("??:0", "??")
    assert loc.file == "??"
    assert loc.line == 0
    assert loc.function == ""
    assert not loc.resolved


def test_parse_discriminator():
    loc = _parse_file_line("/src/loop.cpp:17 (discriminator 1)", "hot_loop")
    assert loc.file == "/src/loop.cpp"
    assert loc.line == 17


def test_unresolved_sentinel():
    loc = _unresolved()
    assert not loc.resolved
    assert loc.file == "??"
    assert loc.line == 0


def test_parse_batch_output_uses_two_lines_per_address():
    output = "\n".join(
        [
            "multiply_bad",
            "/src/matrix_bad.cpp:41",
            "init",
            "/src/matrix_bad.cpp:23",
        ]
    )
    result = _parse_addr2line_output([0x401234, 0x401100], output)
    assert result[0x401234].file == "/src/matrix_bad.cpp"
    assert result[0x401234].line == 41
    assert result[0x401234].function == "multiply_bad"
    assert result[0x401100].function == "init"
