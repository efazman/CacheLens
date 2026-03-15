"""Thin wrapper around the OpenAI chat completions API.

Separated from prompt building and parsing so it can be swapped out
for other providers or a mock in tests.
"""

from __future__ import annotations

import os

from cacheprof.config import Config
from cacheprof.utils.logging import get_logger

log = get_logger(__name__)


def query_llm(prompt: str, config: Config) -> str:
    """Send a prompt to the LLM and return the raw response text.

    Requires OPENAI_API_KEY in the environment.

    Returns:
        Raw response string.

    Raises:
        RuntimeError: If the API call fails.
    """
    api_key = os.environ.get("OPENAI_API_KEY")
    if not api_key:
        raise RuntimeError(
            "OPENAI_API_KEY not set — set it in your environment or "
            "skip the LLM stage with --no-llm"
        )

    try:
        from openai import OpenAI
    except ImportError:
        raise RuntimeError("openai package not installed — pip install openai")

    client = OpenAI(api_key=api_key)

    log.debug("querying %s (max_tokens=%d)", config.llm_model, config.llm_max_tokens)

    response = client.chat.completions.create(
        model=config.llm_model,
        max_tokens=config.llm_max_tokens,
        temperature=0.2,  # low temp for grounded, factual explanations
        messages=[
            {"role": "user", "content": prompt},
        ],
    )

    text = response.choices[0].message.content or ""
    log.debug("LLM response length: %d chars", len(text))
    return text
