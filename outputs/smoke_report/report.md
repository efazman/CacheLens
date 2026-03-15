# CacheScope Report

**Binary:** `benchmarks/matrix_bad`  
**Mode:** COUNTER_SAMPLING  
**Unresolved rate:** 0.0%

## Capability Check

```text
perf: not found
addr2line: not found
debug info: True
mem sampling supported: False
fallback event: cache-misses
```

## Hardware Counters

| Event | Count |
|-------|------:|
| cache-misses | 10 |
| cache-references | 100 |

## Top Hotspots

| # | File:Line | Function | Misses | % Total |
|---|-----------|----------|-------:|--------:|
| 1 | `benchmarks/matrix_bad.cpp:41` | multiply_bad | 10 | 100.0% |

### Hotspot #1 — `benchmarks/matrix_bad.cpp:41`

**test**

why

> **Suggestion:** fix

> Parse OK: True

```cpp
41 -> line
```
