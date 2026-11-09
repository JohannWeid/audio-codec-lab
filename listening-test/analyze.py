#!/usr/bin/env python3
"""Evaluates the results of the listening test as ITU-R BS.1534-3 recommends. See the README."""
from __future__ import annotations

import argparse
import csv
import itertools
import sys
from dataclasses import dataclass
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "eval"))

import plots  # noqa: E402

HIDDEN_REFERENCE = "reference"
MID_ANCHOR = "anchor70"
LOW_ANCHOR = "anchor35"
SCORE_LIMIT = 90
LARGEST_SHARE_OF_ITEMS = 0.15
ANCHOR_ITEM_LIMIT = 0.25
TRAINING_PREFIX = "training"
TRIAL_PREFIX = "trial_"
RESAMPLES = 10000
SEED = 1534
MOST_LISTENERS_FOR_EXACT_TEST = 16
COMPARED_PAIRS = (
    ("model_32k", "baseline_32k"),
    ("model_64k", "baseline_64k"),
    ("model_32k", "model_32k_old_limit"),
)


@dataclass(frozen=True)
class Rating:
    listener: str
    item: str
    condition: str
    score: float


def read_ratings(path: Path) -> list[Rating]:
    """Reads the file mushra.csv of webMUSHRA and leaves out the training.

    Newer versions of webMUSHRA give each session an id. The release 1.4.3 does not, so there the
    listeners are told apart by the short name they typed in, which must not occur twice.
    """
    with open(path, newline="", encoding="utf-8") as file:
        rows = list(csv.DictReader(file))
    ratings = []
    seen = set()
    for row in rows:
        trial = row["trial_id"]
        if trial.startswith(TRAINING_PREFIX):
            continue
        if not trial.startswith(TRIAL_PREFIX):
            raise ValueError(f"unknown trial {trial}")
        rating = Rating(
            row["session_uuid"] if "session_uuid" in row else row["listener"],
            trial[len(TRIAL_PREFIX) :],
            row["rating_stimulus"],
            float(row["rating_score"]),
        )
        key = (rating.listener, rating.item, rating.condition)
        if key in seen:
            raise ValueError(f"listener {rating.listener} rated {rating.item} twice")
        seen.add(key)
        ratings.append(rating)
    return ratings


def items_of(ratings: list[Rating]) -> list[str]:
    return sorted({r.item for r in ratings})


def conditions_of(ratings: list[Rating]) -> list[str]:
    return sorted({r.condition for r in ratings})


def listeners_of(ratings: list[Rating]) -> list[str]:
    return sorted({r.listener for r in ratings})


def scores_by_listener(
    ratings: list[Rating],
    condition: str,
    listeners: list[str],
    items: list[str],
) -> np.ndarray:
    """An array of listeners by items, with NaN where a rating is missing."""
    result = np.full((len(listeners), len(items)), np.nan)
    row = {name: i for i, name in enumerate(listeners)}
    column = {name: i for i, name in enumerate(items)}
    for rating in ratings:
        if rating.condition == condition and rating.item in column and rating.listener in row:
            result[row[rating.listener], column[rating.item]] = rating.score
    return result


@dataclass
class Screening:
    kept: list[str]
    excluded: dict[str, str]
    uninformative_anchor_items: list[str]


def post_screen(ratings: list[Rating]) -> Screening:
    """The rules of section 4.1.2 of BS.1534-3, with their limit of 15 % of the items.

    A listener is excluded for rating the hidden reference below 90, or the mid anchor above 90, in
    more than that share of the items. If over 25 % of the listeners rate the mid anchor above 90 on
    an item, then the anchor did not degrade that item enough, and it does not count against anyone.
    """
    listeners = listeners_of(ratings)
    items = items_of(ratings)
    reference = scores_by_listener(ratings, HIDDEN_REFERENCE, listeners, items)
    anchor = scores_by_listener(ratings, MID_ANCHOR, listeners, items)

    high = anchor > SCORE_LIMIT
    uninformative = np.mean(high, axis=0) > ANCHOR_ITEM_LIMIT
    excluded = {}
    for index, listener in enumerate(listeners):
        low_share = float(np.mean(reference[index] < SCORE_LIMIT))
        high_share = float(np.sum(high[index] & ~uninformative) / len(items))
        reasons = []
        if low_share > LARGEST_SHARE_OF_ITEMS:
            reasons.append(f"hidden reference below 90 on {100 * low_share:.0f} % of the items")
        if high_share > LARGEST_SHARE_OF_ITEMS:
            reasons.append(f"mid anchor above 90 on {100 * high_share:.0f} % of the items")
        if reasons:
            excluded[listener] = ", ".join(reasons)
    kept = [listener for listener in listeners if listener not in excluded]
    return Screening(
        kept, excluded, [item for item, flag in zip(items, uninformative) if flag]
    )


def bootstrap_interval(values: np.ndarray, rng: np.random.Generator) -> tuple[float, float]:
    """The 95 % interval of the mean, from resampling the values with replacement."""
    if len(values) == 0:
        return float("nan"), float("nan")
    picks = rng.integers(0, len(values), size=(RESAMPLES, len(values)))
    means = np.mean(values[picks], axis=1)
    low, high = np.percentile(means, [2.5, 97.5])
    return float(low), float(high)


def permutation_p_value(differences: np.ndarray, rng: np.random.Generator) -> float:
    """Two-sided test of randomization, which BS.1534-3 recommends, for paired differences.

    If two conditions did not differ, then each difference could have had either sign. The p-value is
    the share of all sign assignments whose mean is at least as far from zero as the observed one.
    """
    count = len(differences)
    observed = abs(float(np.mean(differences)))
    if count <= MOST_LISTENERS_FOR_EXACT_TEST:
        signs = np.array(list(itertools.product((-1.0, 1.0), repeat=count)))
    else:
        signs = rng.choice((-1.0, 1.0), size=(RESAMPLES, count))
    means = np.abs(np.mean(signs * differences, axis=1))
    # A small allowance, so that a tie is not lost to rounding.
    return float(np.mean(means >= observed - 1e-12))


@dataclass
class ConditionSummary:
    condition: str
    mean: float
    low: float
    high: float
    median: float
    q1: float
    q3: float
    count: int


def summarize(values: np.ndarray, name: str, rng: np.random.Generator) -> ConditionSummary:
    values = values[~np.isnan(values)]
    low, high = bootstrap_interval(values, rng)
    q1, median, q3 = np.percentile(values, [25, 50, 75])
    return ConditionSummary(
        name, float(np.mean(values)), low, high, float(median), float(q1), float(q3), len(values)
    )


@dataclass
class Comparison:
    first: str
    second: str
    mean_difference: float
    low: float
    high: float
    p_value: float
    listeners: int


def compare(
    ratings: list[Rating],
    first: str,
    second: str,
    listeners: list[str],
    items: list[str],
    rng: np.random.Generator,
) -> Comparison:
    """The difference per listener, averaged over the items.

    The listeners are the unit, since several ratings by one person are not independent.
    """
    a = scores_by_listener(ratings, first, listeners, items)
    b = scores_by_listener(ratings, second, listeners, items)
    differences = np.nanmean(a - b, axis=1)
    differences = differences[~np.isnan(differences)]
    low, high = bootstrap_interval(differences, rng)
    return Comparison(
        first,
        second,
        float(np.mean(differences)),
        low,
        high,
        permutation_p_value(differences, rng),
        len(differences),
    )


def write_report(
    path: Path,
    screening: Screening,
    overall: list[ConditionSummary],
    per_item: dict[str, list[ConditionSummary]],
    comparisons: list[Comparison],
) -> None:
    lines = ["# Results of the listening test", ""]
    total = len(screening.kept) + len(screening.excluded)
    lines += [f"Generated by `listening-test/analyze.py` from {total} listeners.", ""]
    lines += [f"Excluded by the post-screening: {len(screening.excluded)}.", ""]
    for reason in screening.excluded.values():
        lines += [f"- {reason}"]
    if screening.excluded:
        lines += [""]
    if not screening.kept:
        lines += ["No listener is left, so there is nothing to evaluate.", ""]
        path.write_text("\n".join(lines), encoding="utf-8")
        return
    if screening.uninformative_anchor_items:
        names = ", ".join(screening.uninformative_anchor_items)
        lines += [f"Items where the mid anchor was rated too high to count: {names}.", ""]

    lines += ["## Comparisons at the same bitrate", ""]
    lines += ["A positive difference means that the first condition got the higher score.", ""]
    lines += ["| Comparison | Mean difference | 95 % interval | p | Listeners |"]
    lines += ["|---|---:|---:|---:|---:|"]
    for c in comparisons:
        lines += [
            f"| {c.first} - {c.second} | {c.mean_difference:+.1f} | "
            f"{c.low:+.1f} to {c.high:+.1f} | {c.p_value:.3f} | {c.listeners} |"
        ]
    lines += [""]

    def table(title: str, rows: list[ConditionSummary]) -> list[str]:
        result = [f"## {title}", ""]
        result += ["| Condition | Mean | 95 % interval | Median | Quartiles | Ratings |"]
        result += ["|---|---:|---:|---:|---:|---:|"]
        for s in rows:
            result += [
                f"| {s.condition} | {s.mean:.1f} | {s.low:.1f} to {s.high:.1f} | {s.median:.1f} | "
                f"{s.q1:.1f} to {s.q3:.1f} | {s.count} |"
            ]
        return result + [""]

    lines += table("All items", overall)
    for item, rows in per_item.items():
        lines += table(f"Item {item}", rows)
    path.write_text("\n".join(lines), encoding="utf-8")


def plot_results(
    path: Path,
    overall: list[ConditionSummary],
    per_item: dict[str, list[ConditionSummary]],
    listeners: int,
) -> None:
    """Means with their intervals: one panel for all items and one for each item."""
    panels = {"All items": overall, **per_item}
    columns = 4
    rows = int(np.ceil(len(panels) / columns))
    figure, all_axes = plots.plt.subplots(
        rows,
        columns,
        figsize=(13, 3.1 * rows + 0.9),
        sharey=True,
        facecolor=plots.SURFACE,
        squeeze=False,
    )
    flat = all_axes.flatten()
    order = [s.condition for s in overall]
    for axes, (title, summaries) in zip(flat, panels.items()):
        plots.style_axes(axes)
        by_name = {s.condition: s for s in summaries}
        present = [name for name in order if name in by_name]
        positions = np.arange(len(present))
        means = np.array([by_name[n].mean for n in present])
        below = means - np.array([by_name[n].low for n in present])
        above = np.array([by_name[n].high for n in present]) - means
        axes.errorbar(
            means,
            positions,
            xerr=[below, above],
            fmt="o",
            color=plots.NOISE_COLOR,
            ecolor=plots.NOISE_COLOR,
            elinewidth=2,
            markersize=6,
            markeredgecolor=plots.SURFACE,
            markeredgewidth=1.2,
        )
        axes.set_yticks(positions)
        axes.set_yticklabels(present)
        axes.set_xlim(0, 100)
        axes.set_ylim(len(present) - 0.5, -0.5)
        axes.set_title(title.replace("_", " "), loc="left", fontsize=10, color=plots.INK)
        axes.set_xlabel("Score, 100 means no audible difference", color=plots.INK_SECONDARY, fontsize=8)
    for axes in flat[len(panels) :]:
        axes.set_visible(False)
    top = plots.headline(
        figure,
        "Listening test: mean score with 95 % interval",
        f"MUSHRA following ITU-R BS.1534-3, {listeners} listeners after post-screening.",
    )
    figure.tight_layout(rect=(0, 0, 1, top))
    figure.savefig(path, dpi=150, facecolor=plots.SURFACE)
    plots.plt.close(figure)


def analyze(ratings: list[Rating], out: Path) -> tuple[Screening, list[Comparison]]:
    rng = np.random.default_rng(SEED)
    screening = post_screen(ratings)
    out.mkdir(parents=True, exist_ok=True)
    if not screening.kept:
        write_report(out / "results.md", screening, [], {}, [])
        return screening, []
    kept = [r for r in ratings if r.listener in set(screening.kept)]
    items = items_of(kept)
    conditions = conditions_of(kept)

    # All items together: first the mean of each listener, so that the listeners are the unit.
    overall = []
    for condition in conditions:
        scores = scores_by_listener(kept, condition, screening.kept, items)
        overall.append(summarize(np.nanmean(scores, axis=1), condition, rng))
    overall.sort(key=lambda s: -s.mean)

    per_item = {}
    for item in items:
        rows = []
        for condition in conditions:
            scores = scores_by_listener(kept, condition, screening.kept, [item])
            if not np.all(np.isnan(scores)):
                rows.append(summarize(scores[:, 0], condition, rng))
        per_item[item] = rows

    comparisons = [
        compare(kept, first, second, screening.kept, items, rng)
        for first, second in COMPARED_PAIRS
        if first in conditions and second in conditions
    ]
    out.mkdir(parents=True, exist_ok=True)
    write_report(out / "results.md", screening, overall, per_item, comparisons)
    plot_results(out / "scores.png", overall, per_item, len(screening.kept))
    return screening, comparisons


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv", type=Path, help="the file mushra.csv of webMUSHRA")
    parser.add_argument("--out", type=Path, default=HERE / "results", help="folder for the results")
    arguments = parser.parse_args()

    screening, comparisons = analyze(read_ratings(arguments.csv), arguments.out)
    print(f"{len(screening.kept)} listeners kept, {len(screening.excluded)} excluded.")
    for c in comparisons:
        print(
            f"{c.first} - {c.second}: {c.mean_difference:+.1f} points, "
            f"95 % interval {c.low:+.1f} to {c.high:+.1f}, p = {c.p_value:.3f}"
        )
    print(f"The results are in {arguments.out}")


if __name__ == "__main__":
    main()
