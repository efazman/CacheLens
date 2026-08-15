"""CLI entry point for cacheprof."""

from __future__ import annotations

import sys
from pathlib import Path

import click

from cacheprof.capabilities import check_capabilities, format_capability_report
from cacheprof.config import Config
from cacheprof.utils.logging import setup as setup_logging


@click.group()
@click.option("--verbose", "-v", is_flag=True, help="Enable debug logging.")
@click.pass_context
def main(ctx: click.Context, verbose: bool) -> None:
    """CacheLens — cache locality profiler (Python prototype)."""
    setup_logging(verbose)
    ctx.ensure_object(dict)
    ctx.obj["config"] = Config(verbose=verbose)


@main.command()
@click.argument("binary", type=click.Path(exists=True))
@click.pass_context
def check(ctx: click.Context, binary: str) -> None:
    """Run capability checks against BINARY without profiling."""
    config: Config = ctx.obj["config"]
    report = check_capabilities(binary, config)
    click.echo(format_capability_report(report))
    if not report.can_proceed:
        sys.exit(1)


@main.command()
@click.argument("binary", type=click.Path(exists=True))
@click.argument("args", nargs=-1)
@click.pass_context
def profile(ctx: click.Context, binary: str, args: tuple[str, ...]) -> None:
    """Profile BINARY for cache locality bottlenecks."""
    config: Config = ctx.obj["config"]

    # The runner handles capability checks, partial failure, and reporting.
    from cacheprof.runner import run_pipeline
    from cacheprof.report.terminal import print_report

    report = run_pipeline(binary, list(args), config)
    print_report(report)

    if not report.capabilities.can_proceed:
        sys.exit(1)

    if report.run_dir:
        click.echo(f"\nArtifacts saved to: {report.run_dir}")


if __name__ == "__main__":
    main()
