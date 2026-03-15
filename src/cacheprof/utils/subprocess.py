"""Subprocess helpers with consistent error handling and logging."""

from __future__ import annotations

import subprocess
from dataclasses import dataclass
from typing import Optional

from cacheprof.utils.logging import get_logger

log = get_logger(__name__)


@dataclass
class RunResult:
    """Wrapper around subprocess.CompletedProcess with convenience methods."""
    returncode: int
    stdout: str
    stderr: str
    command: list[str]

    @property
    def ok(self) -> bool:
        return self.returncode == 0


def run(
    cmd: list[str],
    *,
    timeout: Optional[int] = None,
    check: bool = False,
    input_data: Optional[str] = None,
    capture: bool = True,
) -> RunResult:
    """Run a command and return a RunResult.

    Args:
        cmd: Command and arguments.
        timeout: Seconds before SIGKILL.
        check: If True, raise on non-zero exit.
        input_data: String to pipe into stdin.
        capture: Capture stdout/stderr (disable for long-running procs you stream).
    """
    log.debug("exec: %s", " ".join(cmd))
    try:
        proc = subprocess.run(
            cmd,
            capture_output=capture,
            text=True,
            timeout=timeout,
            input=input_data,
            check=check,
        )
        result = RunResult(
            returncode=proc.returncode,
            stdout=proc.stdout or "",
            stderr=proc.stderr or "",
            command=cmd,
        )
    except FileNotFoundError:
        log.warning("binary not found: %s", cmd[0])
        result = RunResult(returncode=-1, stdout="", stderr=f"{cmd[0]}: not found", command=cmd)
    except subprocess.TimeoutExpired:
        log.warning("timeout after %ds: %s", timeout, " ".join(cmd))
        result = RunResult(returncode=-1, stdout="", stderr="timeout", command=cmd)
    except subprocess.CalledProcessError as exc:
        result = RunResult(
            returncode=exc.returncode,
            stdout=exc.stdout or "",
            stderr=exc.stderr or "",
            command=cmd,
        )

    if not result.ok:
        log.debug("exit %d | stderr: %s", result.returncode, result.stderr[:300])
    return result
