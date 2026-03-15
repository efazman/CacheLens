"""Build LLM prompts from hotspot data and the prompt template."""

from __future__ import annotations

from pathlib import Path

from cacheprof.models import CollectionMode, Hotspot, HotspotSignals

_TEMPLATE_PATH = Path(__file__).parent.parent.parent.parent / "prompts" / "hotspot_explainer.txt"


def build_prompt(
    hotspot: Hotspot,
    signals: HotspotSignals,
    collection_mode: CollectionMode,
) -> str:
    """Fill the prompt template with concrete hotspot data."""
    template = _load_template()

    replacements = {
        "{{collection_mode}}": collection_mode.name,
        "{{llc_miss_rate}}": f"{signals.llc_miss_rate:.4f}",
        "{{hotspot_concentration}}": f"{signals.hotspot_concentration:.4f}",
        "{{has_nested_loops}}": str(signals.has_nested_loops),
        "{{has_pointer_deref}}": str(signals.has_pointer_deref),
        "{{dataset_size_hint}}": str(signals.dataset_size_hint or "unknown"),
        "{{unresolved_sample_rate}}": f"{signals.unresolved_sample_rate:.2f}",
        "{{snippet}}": hotspot.source_snippet or "(no source available)",
        "{{function}}": hotspot.location.function or "??",
        "{{file}}": hotspot.location.file,
        "{{line}}": str(hotspot.location.line),
    }

    result = template
    for placeholder, value in replacements.items():
        result = result.replace(placeholder, value)

    return result


def _load_template() -> str:
    """Load the prompt template, with a fallback if the file is missing."""
    if _TEMPLATE_PATH.exists():
        return _TEMPLATE_PATH.read_text(encoding="utf-8")

    # Fallback: minimal inline prompt
    return (
        "You are a cache performance expert. Analyze this hotspot and respond "
        "in JSON with fields: summary, explanation, suggestion.\n\n"
        "Collection mode: {{collection_mode}}\n"
        "LLC miss rate: {{llc_miss_rate}}\n"
        "Hotspot concentration: {{hotspot_concentration}}\n"
        "Snippet:\n{{snippet}}\n"
    )
