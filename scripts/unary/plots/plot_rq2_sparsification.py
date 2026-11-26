#!/usr/bin/env python3
"""Plot the RQ2 quotient-sparsification ablation from the experiment CSVs.

The two comparison panels use medians of successful repetitions from
``rq1-results.csv``.  Direct runs that exhausted memory are placed in a
separate strip because their x coordinate is unknown.  The current experiment
covers the taint suite only.  The preprocessing panel is kept in the code but
disabled because the figure now contains only the first two panels.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import statistics
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

import matplotlib

matplotlib.use("Agg")

import matplotlib.pyplot as plt
from matplotlib import font_manager
from matplotlib.lines import Line2D
from matplotlib.ticker import LogLocator, NullFormatter


# ---------------------------------------------------------------------------
# Global figure configuration
# ---------------------------------------------------------------------------

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR.parent))
from rq2 import config_rq2 as config

RESULTS_CSV = config.ABLATION_CSV
BREAKDOWN_CSV = config.OUTPUT_CSV
OUTPUT_PDF = SCRIPT_DIR / "rq2-sparsification.pdf"

# Two comparison panels are plotted.  The former three-panel size was
# FIGURE_SIZE_IN = (7.35, 2.72) and TAINT_FIGURE_SIZE_IN = (6.3, 2.72).
FIGURE_SIZE_IN = (4.65, 2.72)
TAINT_FIGURE_SIZE_IN = (4.65, 2.72)
FIGURE_DPI = 200
# The former third ratio (0.72) belongs to the disabled preprocessing panel.
# PANEL_WIDTH_RATIOS = (1.0, 1.0, 0.72)
PANEL_WIDTH_RATIOS = (1.0, 1.0)
MO_STRIP_WIDTH_RATIO = 0.105
OUTER_WSPACE = 0.40
MO_STRIP_WSPACE = 0.045
MARGINS = dict(left=0.075, right=0.992, bottom=0.195, top=0.855)

FONT_FAMILY = "Times New Roman"
FONT_SIZE = 10.0
TITLE_SIZE = 11.5
LABEL_SIZE = 10.5
TICK_SIZE = 9.5
ANNOTATION_SIZE = 8.8
LEGEND_SIZE = 9.5

SUITE_STYLE = {
    "taint": {"label": "Taint", "color": "#2878B5", "marker": "o"},
    "valueflow": {"label": "Value-flow", "color": "#E07A24", "marker": "s"},
}
POINT_SIZE = 22
POINT_ALPHA = 0.90
POINT_EDGE_COLOR = "white"
POINT_EDGE_WIDTH = 0.55
MO_POINT_SIZE = 23
MO_STRIP_FACE = "#ECEFF1"
MO_STRIP_EDGE = "#B0B6BC"

GRID_COLOR = "#D9DDE2"
GRID_WIDTH = 0.45
GUIDE_COLOR = "#9298A0"
GUIDE_WIDTH = 0.65
GUIDE_DASHES = (3.0, 2.2)
MEDIAN_COLOR = "#A33A36"
MEDIAN_WIDTH = 0.85
AXIS_COLOR = "#33373B"

TIME_LIMITS = (8e-3, 2.1e2)
RSS_LIMITS = (8e-3, 8.0e1)
TAINT_TIME_LIMITS = (8e-3, 3.0e1)
TAINT_RSS_LIMITS = (8e-3, 1.2e1)
PREPROCESS_LIMITS = (1e-2, 4.0)
SPEEDUP_GUIDES = (1, 10, 100, 1000)

# Labels are deliberately sparse.  Set to an empty tuple for a label-free plot.
# ANNOTATED_PROJECTS = ("phospy", "leela", "nab")
ANNOTATED_PROJECTS = ()
ANNOTATION_OFFSETS = {
    "time": {"phospy": (-4, 5), "leela": (-5, 5), "nab": (-4, -9)},
    "rss": {"phospy": (-4, 5), "leela": (-5, 5), "nab": (-4, 5)},
}

PDF_METADATA = {
    "Title": "RQ2 quotient-sparsification ablation",
    "Subject": "Direct versus sparsified ACF runtime and memory (taint)",
    "Creator": "scripts/unary/plot_rq2_sparsification.py",
}


@dataclass(frozen=True)
class Measurement:
    suite: str
    project: str
    acf_time_s: float
    acf_rss_gib: float
    direct_time_s: float | None
    direct_rss_gib: float | None
    direct_mo: bool
    preprocessing_pct: float


def read_csv(path: Path) -> list[dict[str, str]]:
    if not path.is_file():
        raise FileNotFoundError(f"missing input data: {path}")
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def parse_list(row: dict[str, str], field: str) -> list:
    value = json.loads(row[field])
    if not isinstance(value, list):
        raise ValueError(f"{field} is not a list for {row.get('dot_name')}")
    return value


def median_successful(row: dict[str, str], field: str) -> float | None:
    statuses = parse_list(row, "statuses")
    values = parse_list(row, field)
    if len(statuses) != len(values):
        raise ValueError(f"status/value length mismatch for {row['dot_name']}:{field}")
    successful = [float(value) for status, value in zip(statuses, values) if status == "success"]
    return statistics.median(successful) if successful else None


def ratio_of_medians_pct(row: dict[str, str], numerator: str, denominator: str) -> float:
    numerators = parse_list(row, numerator)
    denominators = parse_list(row, denominator)
    if len(numerators) != len(denominators):
        raise ValueError(f"phase length mismatch for {row['dot_name']}")
    valid_pairs = [(float(n), float(d)) for n, d in zip(numerators, denominators) if float(d) > 0]
    if not valid_pairs:
        raise ValueError(f"no valid phase ratios for {row['dot_name']}")
    # Match the paper's per-project convention: ratio of the two reported medians.
    return 100.0 * statistics.median(n for n, _ in valid_pairs) / statistics.median(
        d for _, d in valid_pairs
    )


def load_measurements() -> list[Measurement]:
    results = read_csv(RESULTS_CSV)
    breakdown = read_csv(BREAKDOWN_CSV)

    by_key_and_experiment: dict[tuple[str, str, str], dict[str, str]] = {}
    for row in results:
        key = (row["dataset"], row["dot_name"], row["experiment"])
        if key in by_key_and_experiment:
            raise ValueError(f"duplicate result row: {key}")
        by_key_and_experiment[key] = row

    phase_by_key: dict[tuple[str, str], dict[str, str]] = {}
    for row in breakdown:
        key = (row["dataset"], row["dot_name"])
        if key in phase_by_key:
            raise ValueError(f"duplicate breakdown row: {key}")
        phase_by_key[key] = row

    measurements: list[Measurement] = []
    subject_keys = sorted({(r["dataset"], r["dot_name"]) for r in results})
    for suite, dot_name in subject_keys:
        adaptive = by_key_and_experiment.get((suite, dot_name, "adaptive"))
        direct = by_key_and_experiment.get((suite, dot_name, "adaptive-direct"))
        phase = phase_by_key.get((suite, dot_name))
        if adaptive is None or direct is None or phase is None:
            raise ValueError(f"incomplete RQ2 data for {(suite, dot_name)}")

        acf_time = median_successful(adaptive, "wall_time_seconds")
        acf_rss_bytes = median_successful(adaptive, "peak_rss_bytes")
        direct_time = median_successful(direct, "wall_time_seconds")
        direct_rss_bytes = median_successful(direct, "peak_rss_bytes")
        if acf_time is None or acf_rss_bytes is None:
            raise ValueError(f"ACF did not complete for {(suite, dot_name)}")

        direct_statuses = parse_list(direct, "statuses")
        direct_mo = direct_time is None and "memory-limit" in direct_statuses
        if direct_time is None and not direct_mo:
            raise ValueError(f"unexpected Direct failure for {(suite, dot_name)}")

        measurements.append(
            Measurement(
                suite=suite,
                project=dot_name.removesuffix(".dot"),
                acf_time_s=acf_time,
                acf_rss_gib=acf_rss_bytes / (1024.0**3),
                direct_time_s=direct_time,
                direct_rss_gib=None if direct_rss_bytes is None else direct_rss_bytes / (1024.0**3),
                direct_mo=direct_mo,
                preprocessing_pct=ratio_of_medians_pct(
                    phase, "quotient_sparsification_us", "total_time_us"
                ),
            )
        )

    if not measurements:
        raise ValueError("no benchmark subjects found")
    return measurements


def configure_matplotlib() -> None:
    # Register newly installed fonts even when Matplotlib's on-disk font cache
    # predates the Times New Roman installation.  Refuse silent substitution.
    for font_path in font_manager.findSystemFonts():
        if "times" in Path(font_path).stem.lower():
            font_manager.fontManager.addfont(font_path)
    font_manager.findfont(FONT_FAMILY, fallback_to_default=False)

    plt.rcParams.update(
        {
            "font.family": FONT_FAMILY,
            "font.size": FONT_SIZE,
            "axes.titlesize": TITLE_SIZE,
            "axes.labelsize": LABEL_SIZE,
            "xtick.labelsize": TICK_SIZE,
            "ytick.labelsize": TICK_SIZE,
            "legend.fontsize": LEGEND_SIZE,
            "axes.edgecolor": AXIS_COLOR,
            "axes.linewidth": 0.65,
            "xtick.major.width": 0.55,
            "ytick.major.width": 0.55,
            "xtick.minor.width": 0.40,
            "ytick.minor.width": 0.40,
            "pdf.fonttype": 42,
            "ps.fonttype": 42,
        }
    )


def scatter_by_suite(ax, rows: Iterable[Measurement], x_attr: str, y_attr: str) -> None:
    rows = list(rows)
    for suite, style in SUITE_STYLE.items():
        selected = [row for row in rows if row.suite == suite]
        ax.scatter(
            [getattr(row, x_attr) for row in selected],
            [getattr(row, y_attr) for row in selected],
            s=POINT_SIZE,
            marker=style["marker"],
            c=style["color"],
            alpha=POINT_ALPHA,
            edgecolors=POINT_EDGE_COLOR,
            linewidths=POINT_EDGE_WIDTH,
            zorder=3,
        )


def add_speedup_guides(ax, limits: tuple[float, float], speedups=SPEEDUP_GUIDES) -> None:
    lower, upper = limits
    for speedup in speedups:
        x0 = max(lower, lower * speedup)
        x1 = upper
        if x0 >= x1:
            continue
        ax.plot(
            [x0, x1],
            [x0 / speedup, x1 / speedup],
            color=GUIDE_COLOR,
            linewidth=GUIDE_WIDTH,
            dashes=GUIDE_DASHES,
            zorder=1,
        )
        # Keep the larger guide labels clear of the top and right spines.
        label_x = upper / 1.45
        label_y = label_x / speedup
        if lower * 1.15 < label_y < upper / 1.15:
            label = "equal" if speedup == 1 else f"{speedup}×"
            ax.text(
                label_x,
                label_y,
                label,
                color=GUIDE_COLOR,
                fontsize=ANNOTATION_SIZE,
                ha="right",
                va="bottom",
                rotation=math.degrees(math.atan(1 / speedup)),
                transform_rotates_text=True,
                rotation_mode="anchor",
            )


def style_log_comparison_axis(ax, limits: tuple[float, float]) -> None:
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlim(limits)
    ax.set_ylim(limits)
    ax.grid(True, which="major", color=GRID_COLOR, linewidth=GRID_WIDTH, zorder=0)
    ax.xaxis.set_major_locator(LogLocator(base=10, numticks=100))
    ax.yaxis.set_major_locator(LogLocator(base=10, numticks=100))
    ax.xaxis.set_minor_locator(LogLocator(base=10, subs=range(2, 10), numticks=100))
    ax.yaxis.set_minor_locator(LogLocator(base=10, subs=range(2, 10), numticks=100))
    ax.xaxis.set_minor_formatter(NullFormatter())
    ax.yaxis.set_minor_formatter(NullFormatter())


def annotate_projects(ax, rows: Iterable[Measurement], x_attr: str, y_attr: str, panel: str) -> None:
    for row in rows:
        if row.project not in ANNOTATED_PROJECTS:
            continue
        offset = ANNOTATION_OFFSETS[panel].get(row.project, (3, 3))
        ax.annotate(
            row.project,
            (getattr(row, x_attr), getattr(row, y_attr)),
            xytext=offset,
            textcoords="offset points",
            fontsize=ANNOTATION_SIZE,
            ha="right" if offset[0] < 0 else "left",
            va="bottom" if offset[1] >= 0 else "top",
            color="#3F4449",
            zorder=4,
        )


def style_mo_axis(ax, y_limits: tuple[float, float], mo_count: int) -> None:
    ax.set_facecolor(MO_STRIP_FACE)
    ax.set_yscale("log")
    ax.set_ylim(y_limits)
    ax.set_xlim(0, 1)
    ax.set_xticks([])
    ax.tick_params(axis="y", which="both", left=False, labelleft=False)
    for side in ("left", "right", "top", "bottom"):
        ax.spines[side].set_color(MO_STRIP_EDGE)
        ax.spines[side].set_linewidth(0.55)
    ax.set_title(f"MO\n({mo_count})", fontsize=ANNOTATION_SIZE, pad=2.0, color="#596067")


def add_mo_points(ax, rows: Iterable[Measurement], y_attr: str) -> None:
    mo_rows = [row for row in rows if row.direct_mo]
    for suite, style in SUITE_STYLE.items():
        selected = [row for row in mo_rows if row.suite == suite]
        ax.scatter(
            [0.5] * len(selected),
            [getattr(row, y_attr) for row in selected],
            s=MO_POINT_SIZE,
            marker=style["marker"],
            c=style["color"],
            alpha=POINT_ALPHA,
            edgecolors=POINT_EDGE_COLOR,
            linewidths=POINT_EDGE_WIDTH,
            zorder=3,
        )


def ecdf(values: Iterable[float]) -> tuple[list[float], list[float]]:
    ordered = sorted(values)
    return ordered, [(index + 1) / len(ordered) for index in range(len(ordered))]


def plot_preprocessing_ecdf(ax, rows: list[Measurement]) -> None:
    for suite, style in SUITE_STYLE.items():
        xs, ys = ecdf(row.preprocessing_pct for row in rows if row.suite == suite)
        ax.step(xs, ys, where="post", color=style["color"], linewidth=1.25, alpha=0.95)
        ax.scatter(
            xs,
            ys,
            s=7,
            marker=style["marker"],
            c=style["color"],
            edgecolors="none",
            zorder=3,
        )

    median = statistics.median(row.preprocessing_pct for row in rows)
    ax.axvline(median, color=MEDIAN_COLOR, linewidth=MEDIAN_WIDTH, linestyle=(0, (3, 2)))
    ax.text(
        median * 1.10,
        0.055,
        f"median {median:.2f}%",
        color=MEDIAN_COLOR,
        fontsize=ANNOTATION_SIZE,
        rotation=90,
        ha="left",
        va="bottom",
    )

    ax.set_xscale("log")
    ax.set_xlim(PREPROCESS_LIMITS)
    ax.set_ylim(0, 1.02)
    ax.set_yticks((0, 0.25, 0.5, 0.75, 1.0), ("0", "25", "50", "75", "100"))
    ax.grid(True, which="major", color=GRID_COLOR, linewidth=GRID_WIDTH, zorder=0)
    ax.xaxis.set_minor_locator(LogLocator(base=10, subs=(2, 5)))
    ax.xaxis.set_minor_formatter(NullFormatter())
    ax.set_title("(c) Preprocessing", loc="left", pad=5)
    ax.set_xlabel("Preprocessing / total time (%)")
    ax.set_ylabel("Inputs (%)")


def make_figure(rows: list[Measurement]):
    configure_matplotlib()
    taint_only = {row.suite for row in rows} == {"taint"}
    figure_size = TAINT_FIGURE_SIZE_IN if taint_only else FIGURE_SIZE_IN
    time_limits = TAINT_TIME_LIMITS if taint_only else TIME_LIMITS
    rss_limits = TAINT_RSS_LIMITS if taint_only else RSS_LIMITS
    speedups = (1, 10, 100) if taint_only else SPEEDUP_GUIDES
    figure = plt.figure(figsize=figure_size, dpi=FIGURE_DPI)
    # Third (preprocessing) panel disabled: keep only the two comparison panels.
    # outer = figure.add_gridspec(1, 3, width_ratios=PANEL_WIDTH_RATIOS, wspace=OUTER_WSPACE)
    outer = figure.add_gridspec(1, 2, width_ratios=PANEL_WIDTH_RATIOS, wspace=OUTER_WSPACE)

    time_grid = outer[0].subgridspec(
        1, 2, width_ratios=(1, MO_STRIP_WIDTH_RATIO), wspace=MO_STRIP_WSPACE
    )
    rss_grid = outer[1].subgridspec(
        1, 2, width_ratios=(1, MO_STRIP_WIDTH_RATIO), wspace=MO_STRIP_WSPACE
    )
    time_ax = figure.add_subplot(time_grid[0])
    time_mo_ax = figure.add_subplot(time_grid[1], sharey=time_ax)
    rss_ax = figure.add_subplot(rss_grid[0])
    rss_mo_ax = figure.add_subplot(rss_grid[1], sharey=rss_ax)
    # pre_ax = figure.add_subplot(outer[2])

    completed = [row for row in rows if not row.direct_mo]
    mo_count = sum(row.direct_mo for row in rows)

    style_mo_axis(time_mo_ax, time_limits, mo_count)
    style_log_comparison_axis(time_ax, time_limits)
    add_speedup_guides(time_ax, time_limits, speedups)
    scatter_by_suite(time_ax, completed, "direct_time_s", "acf_time_s")
    annotate_projects(time_ax, completed, "direct_time_s", "acf_time_s", "time")
    time_ax.set_title("(a) End-to-end time", loc="left", pad=5)
    time_ax.set_xlabel("Direct time (s)")
    time_ax.set_ylabel("ACF time (s)")
    add_mo_points(time_mo_ax, rows, "acf_time_s")

    style_mo_axis(rss_mo_ax, rss_limits, mo_count)
    style_log_comparison_axis(rss_ax, rss_limits)
    add_speedup_guides(rss_ax, rss_limits, speedups)
    scatter_by_suite(rss_ax, completed, "direct_rss_gib", "acf_rss_gib")
    annotate_projects(rss_ax, completed, "direct_rss_gib", "acf_rss_gib", "rss")
    rss_ax.set_title("(b) Peak memory", loc="left", pad=5)
    rss_ax.set_xlabel("Direct RSS (GiB)")
    rss_ax.set_ylabel("ACF RSS (GiB)")
    add_mo_points(rss_mo_ax, rows, "acf_rss_gib")

    # plot_preprocessing_ecdf(pre_ax, rows)

    # Taint-only experiment: the bottom suite legend is no longer needed.
    # legend_handles = [
    #     Line2D(
    #         [0],
    #         [0],
    #         marker=style["marker"],
    #         linestyle="none",
    #         markerfacecolor=style["color"],
    #         markeredgecolor=POINT_EDGE_COLOR,
    #         markeredgewidth=POINT_EDGE_WIDTH,
    #         markersize=5.2,
    #         label=style["label"],
    #     )
    #     for suite, style in SUITE_STYLE.items()
    #     if any(row.suite == suite for row in rows)
    # ]
    # figure.legend(
    #     handles=legend_handles,
    #     loc="lower center",
    #     bbox_to_anchor=(0.5, 0.004),
    #     ncol=len(legend_handles),
    #     frameon=False,
    #     handletextpad=0.45,
    #     columnspacing=1.4,
    # )
    figure.subplots_adjust(**MARGINS)
    return figure


def print_summary(rows: list[Measurement]) -> None:
    completed = [row for row in rows if not row.direct_mo]
    time_speedups = [row.direct_time_s / row.acf_time_s for row in completed]
    rss_reductions = [row.direct_rss_gib / row.acf_rss_gib for row in completed]
    geometric_time = math.exp(statistics.mean(math.log(value) for value in time_speedups))
    geometric_rss = math.exp(statistics.mean(math.log(value) for value in rss_reductions))
    preprocessing = [row.preprocessing_pct for row in rows]
    print(f"subjects: {len(rows)}; joint completions: {len(completed)}; Direct MO: {len(rows) - len(completed)}")
    print(f"geomean Direct/ACF: time {geometric_time:.1f}x, RSS {geometric_rss:.1f}x")
    print(
        "preprocessing share: "
        f"median {statistics.median(preprocessing):.2f}%, "
        f"range {min(preprocessing):.2f}%--{max(preprocessing):.2f}%"
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dataset", choices=("taint", "valueflow"))
    parser.add_argument("--output", type=Path, default=OUTPUT_PDF)
    args = parser.parse_args()
    rows = load_measurements()
    if args.dataset:
        rows = [row for row in rows if row.suite == args.dataset]
    figure = make_figure(rows)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(args.output, bbox_inches="tight", pad_inches=0.02, metadata=PDF_METADATA)
    plt.close(figure)
    print_summary(rows)
    print(f"wrote {args.output}")


if __name__ == "__main__":
    main()
