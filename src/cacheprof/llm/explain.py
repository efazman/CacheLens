"""Generate LLM explanations for hotspots.

Orchestrates: build prompt → call LLM → parse JSON response.
Handles parse failures gracefully (stores raw text, never crashes).
"""

from __future__ import annotations

import json
import re

from cacheprof.config import Config
from cacheprof.models import CollectionMode, Diagnosis, Hotspot, HotspotSignals
from cacheprof.llm.client import query_llm
from cacheprof.llm.prompt import build_prompt
from cacheprof.utils.logging import get_logger

log = get_logger(__name__)


def explain_hotspots(
    hotspots: list[Hotspot],
    signals: list[HotspotSignals],
    collection_mode: CollectionMode,
    config: Config,
) -> list[Diagnosis]:
    """Generate an LLM diagnosis for each hotspot.

    Returns a list of Diagnosis parallel to the input hotspots.
    On any failure (API error, parse error), the Diagnosis contains
    the raw response text and parse_ok=False — we never crash.
    """
    diagnoses: list[Diagnosis] = []

    for i, (hs, sig) in enumerate(zip(hotspots, signals)):
        log.info("explaining hotspot %d/%d: %s:%d", i + 1, len(hotspots),
                 hs.location.file, hs.location.line)
        try:
            prompt = build_prompt(hs, sig, collection_mode)
            raw = query_llm(prompt, config)
            diag = _parse_response(raw)
        except Exception as exc:
            log.warning("LLM explanation failed for hotspot %d: %s", i + 1, exc)
            diag = Diagnosis(
                raw_response=str(exc),
                parse_ok=False,
                summary=f"(LLM error: {exc})",
            )
        diagnoses.append(diag)

    return diagnoses


def _parse_response(raw: str) -> Diagnosis:
    """Parse the LLM JSON response into a Diagnosis.

    Tolerates markdown fences and minor junk around the JSON.
    Falls back to storing raw text if parsing fails.
    """
    cleaned = _strip_fences(raw)

    try:
        data = json.loads(cleaned)
        return Diagnosis(
            summary=data.get("summary", ""),
            explanation=data.get("explanation", ""),
            suggestion=data.get("suggestion", ""),
            raw_response=raw,
            parse_ok=True,
        )
    except (json.JSONDecodeError, TypeError) as exc:
        log.warning("JSON parse failed, storing raw text: %s", exc)
        return Diagnosis(
            summary="(could not parse LLM response)",
            explanation=raw,
            suggestion="",
            raw_response=raw,
            parse_ok=False,
        )


def _strip_fences(text: str) -> str:
    """Remove markdown code fences (```json ... ```) if present."""
    # Match ```json ... ``` or ``` ... ```
    m = re.search(r"```(?:json)?\s*\n?(.*?)\n?\s*```", text, re.DOTALL)
    if m:
        return m.group(1).strip()
    return text.strip()
