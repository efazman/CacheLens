"""Tests for the perf script output parser."""

from pathlib import Path

from cacheprof.perf.parser import parse_script_text, parse_script_file

FIXTURE_DIR = Path(__file__).parent / "fixtures"


def test_parse_basic_lines():
    text = (
        "matrix_bad 12345/12345 [001] 1234567.890123: cache-misses: "
        "0x401234 multiply_bad+0x42 (/home/user/matrix_bad)\n"
    )
    samples = parse_script_text(text)
    assert len(samples) == 1
    s = samples[0]
    assert s.pid == 12345
    assert s.tid == 12345
    assert s.cpu == 1
    assert s.event == "cache-misses"
    assert s.address == 0x401234
    assert s.symbol == "multiply_bad"
    assert s.dso == "/home/user/matrix_bad"


def test_skips_comments():
    text = "# this is a comment\n# another\n"
    assert parse_script_text(text) == []


def test_skips_malformed():
    text = "this is not a valid perf line\n"
    assert parse_script_text(text) == []


def test_parse_fixture_file():
    path = FIXTURE_DIR / "perf_script_sample.txt"
    if not path.exists():
        return  # skip if fixture missing
    samples = parse_script_file(path)
    assert len(samples) > 0
    # All should have event = cache-misses
    for s in samples:
        assert s.event == "cache-misses"


def test_multiple_addresses():
    text = (
        "prog 100/100 [0] 1.0: cache-misses: 0xAABB sym1+0x10 (/bin/prog)\n"
        "prog 100/100 [0] 2.0: cache-misses: 0xCCDD sym2+0x20 (/bin/prog)\n"
    )
    samples = parse_script_text(text)
    assert len(samples) == 2
    assert samples[0].address == 0xAABB
    assert samples[1].address == 0xCCDD


def test_parse_with_extra_fields_before_event():
    text = (
        "prog 100/100 [0] 1.0: PERF_RECORD_MISC_USER cache-misses: "
        "0xAABB sym1+0x10 (/bin/prog)\n"
    )
    samples = parse_script_text(text)
    assert len(samples) == 1
    assert samples[0].event == "cache-misses"
    assert samples[0].address == 0xAABB
