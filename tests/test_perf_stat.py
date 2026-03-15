"""Tests for perf stat parsing."""

from cacheprof.perf.stat import _parse_stat_output


def test_parse_basic_counters_and_duration():
    raw = """
 Performance counter stats for './matrix_bad':

         3,456,789      cache-misses              #   34.567% of all cache refs
        10,000,000      cache-references
     2,150,000,000      instructions              #    1.07  insn per cycle
     2,000,000,000      cycles

       2.345678901 seconds time elapsed
"""
    result = _parse_stat_output(raw)
    assert result.counters["cache-misses"] == 3456789
    assert result.counters["cache-references"] == 10000000
    assert result.counters["instructions"] == 2150000000
    assert result.counters["cycles"] == 2000000000
    assert result.duration_seconds == 2.345678901


def test_parse_decimal_counts_and_skip_unavailable():
    raw = """
         1,234.00      cache-misses:u
      <not counted>    cache-references
       0.123456789 seconds time elapsed
"""
    result = _parse_stat_output(raw)
    assert result.counters["cache-misses:u"] == 1234
    assert "cache-references" not in result.counters
    assert result.duration_seconds == 0.123456789
