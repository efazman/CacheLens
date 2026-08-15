"""Core data models for the cacheprof pipeline.

Every struct that crosses a module boundary is defined here.
This prevents circular imports and gives you one place to see the data shape.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum, auto
from pathlib import Path
from typing import Optional


# ---------------------------------------------------------------------------
# Capability negotiation
# ---------------------------------------------------------------------------

class CollectionMode(Enum):
    """How we will collect cache-miss samples."""
    MEM_SAMPLING = auto()       # perf mem — precise IP attribution (PEBS/IBS/SPE)
    COUNTER_SAMPLING = auto()   # perf record -e cache-misses — PMU skid, reduced precision


@dataclass
class CapabilityReport:
    """Result of the pre-flight capability check."""
    perf_version: Optional[str] = None
    perf_available: bool = False
    addr2line_available: bool = False
    addr2line_version: Optional[str] = None
    available_events: list[str] = field(default_factory=list)
    mem_sampling_supported: bool = False
    collection_mode: CollectionMode = CollectionMode.COUNTER_SAMPLING
    counter_sampling_event: Optional[str] = None
    target_has_debug_info: bool = False
    warnings: list[str] = field(default_factory=list)
    errors: list[str] = field(default_factory=list)

    @property
    def can_proceed(self) -> bool:
        return (
            self.perf_available
            and self.addr2line_available
            and self.target_has_debug_info
            and not self.errors
        )


# ---------------------------------------------------------------------------
# Perf data
# ---------------------------------------------------------------------------

@dataclass
class PerfStatResult:
    """Aggregate counters from perf stat."""
    raw_output: str
    counters: dict[str, int] = field(default_factory=dict)
    # e.g. {"cache-misses": 123456, "cache-references": 9876543, "instructions": ...}
    duration_seconds: float = 0.0


@dataclass
class PerfSample:
    """A single sample from perf script output."""
    pid: int = 0
    tid: int = 0
    cpu: int = 0
    timestamp: float = 0.0
    event: str = ""
    address: int = 0          # instruction pointer
    symbol: str = ""          # raw symbol from perf, may be empty
    dso: str = ""             # shared object / binary path


# ---------------------------------------------------------------------------
# DWARF / source resolution
# ---------------------------------------------------------------------------

@dataclass
class SourceLocation:
    """A resolved source-code location."""
    file: str               # source file path
    line: int               # line number (0 = unresolved)
    function: str = ""      # demangled function name
    is_inlined: bool = False

    @property
    def resolved(self) -> bool:
        return self.file != "??" and self.line > 0


@dataclass
class ResolvedSample:
    """A perf sample with source attribution."""
    sample: PerfSample
    location: SourceLocation


# ---------------------------------------------------------------------------
# Aggregation / ranking
# ---------------------------------------------------------------------------

@dataclass
class Hotspot:
    """An aggregated hotspot: one source location with accumulated miss counts."""
    location: SourceLocation
    miss_count: int = 0
    total_samples: int = 0          # across all events at this location
    fraction_of_total: float = 0.0  # miss_count / total misses in run
    source_snippet: Optional[str] = None  # extracted lines around the hotspot


# ---------------------------------------------------------------------------
# Signal extraction (for the LLM)
# ---------------------------------------------------------------------------

@dataclass
class HotspotSignals:
    """Quantitative signals extracted for one hotspot.  Descriptive, not a diagnosis."""
    llc_miss_rate: float = 0.0
    hotspot_concentration: float = 0.0  # fraction of total misses in this one spot
    has_nested_loops: bool = False
    has_pointer_deref: bool = False
    dataset_size_hint: Optional[int] = None
    unresolved_sample_rate: float = 0.0


# ---------------------------------------------------------------------------
# Top-level report
# ---------------------------------------------------------------------------

@dataclass
class ProfileReport:
    """Everything that goes into the final output."""
    capabilities: CapabilityReport = field(default_factory=CapabilityReport)
    stat_result: Optional[PerfStatResult] = None
    hotspots: list[Hotspot] = field(default_factory=list)
    signals: list[HotspotSignals] = field(default_factory=list)
    unresolved_sample_rate: float = 0.0
    run_dir: Optional[Path] = None
    target_binary: str = ""
    target_args: list[str] = field(default_factory=list)
    collection_mode: CollectionMode = CollectionMode.COUNTER_SAMPLING
