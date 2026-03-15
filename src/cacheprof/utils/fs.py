"""Filesystem helpers — run directory creation, artifact saving."""

from __future__ import annotations

from datetime import datetime
from pathlib import Path


def create_run_dir(base: Path) -> Path:
    """Create a timestamped run directory under *base* and return its path."""
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    base.mkdir(parents=True, exist_ok=True)

    for suffix in [""] + [f"_{i:02d}" for i in range(1, 100)]:
        run_dir = base / f"run_{stamp}{suffix}"
        try:
            run_dir.mkdir()
            return run_dir
        except FileExistsError:
            continue

    raise RuntimeError(f"could not create unique run directory under {base}")


def save_artifact(run_dir: Path, name: str, content: str) -> Path:
    """Write *content* to run_dir/name and return the path."""
    path = run_dir / name
    path.write_text(content, encoding="utf-8")
    return path
