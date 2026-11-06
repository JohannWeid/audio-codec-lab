"""The figures of the evaluation. Colors follow the entity: each codec keeps its color everywhere."""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

import matplotlib

matplotlib.use("Agg")

import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402

import metrics  # noqa: E402

SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK_SECONDARY = "#52514e"
GRID = "#e1e0d9"
AXIS = "#c3c2b7"
SIGNAL_COLOR = "#c3c2b7"
NOISE_COLOR = "#2a78d6"
ABOVE_THRESHOLD_COLOR = "#d03b3b"

CODEC_COLORS = {"model": "#2a78d6", "baseline": "#eb6834", "aac": "#1baf7a", "opus": "#eda100"}
CODEC_MARKERS = {"model": "o", "baseline": "s", "aac": "^", "opus": "D"}
CODEC_NAMES = {
    "model": "With masking model",
    "baseline": "Flat noise (baseline)",
    "aac": "AAC (ffmpeg)",
    "opus": "Opus (libopus)",
}
OWN_CODECS = ("model", "baseline")


def style_axes(axes) -> None:
    axes.set_facecolor(SURFACE)
    axes.grid(True, color=GRID, linewidth=1)
    axes.set_axisbelow(True)
    for side in ("top", "right"):
        axes.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        axes.spines[side].set_color(AXIS)
    axes.tick_params(colors=INK_SECONDARY, labelsize=9, length=0)


def headline(figure, title: str, subtitle: str) -> float:
    """Writes the two lines at the top and returns the height that is left for the plots."""
    height = figure.get_figheight()
    figure.text(
        0.012, 1 - 0.14 / height, title, fontsize=13, fontweight="bold", color=INK, va="top"
    )
    figure.text(0.012, 1 - 0.42 / height, subtitle, fontsize=9.5, color=INK_SECONDARY, va="top")
    return 1 - 0.68 / height


def _band_ticks(axes, edges: np.ndarray) -> None:
    """The bands get equal width, close to the Bark scale, and the axis names their frequency."""
    hertz = edges * (metrics.SAMPLE_RATE / 2) / metrics.FRAME_LENGTH
    wanted = [100, 500, 1000, 2000, 4000, 8000, 16000]
    positions = [int(np.argmin(np.abs(hertz - value))) for value in wanted]
    axes.set_xticks(positions)
    axes.set_xticklabels([f"{hertz[p] / 1000:.1f}" for p in positions])
    axes.set_xlim(0, len(edges) - 1)
    axes.set_xlabel("Frequency of the band edge in kHz", color=INK_SECONDARY, fontsize=9)


@dataclass
class BandLevels:
    """Mean energy per band over all frames, for one codec."""

    name: str
    noise: np.ndarray
    mean_excess_db: float


def plot_masking(
    path: Path,
    item: str,
    bitrate: int,
    edges: np.ndarray,
    signal: np.ndarray,
    threshold: np.ndarray,
    panels: list[BandLevels],
) -> None:
    """Signal, threshold and noise per band, one panel per codec, on one common scale."""
    figure, all_axes = plt.subplots(1, len(panels), figsize=(11, 4.9), sharey=True, facecolor=SURFACE)
    positions = np.arange(len(edges))
    threshold_db = metrics.level_db_spl(threshold)
    signal_db = metrics.level_db_spl(signal)
    lowest = float(np.floor(min(threshold_db.min(), signal_db.min()) / 10) * 10) - 10
    highest = float(np.ceil(signal_db.max() / 10) * 10) + 5

    for axes, panel in zip(all_axes, panels):
        style_axes(axes)
        noise_db = metrics.level_db_spl(panel.noise)
        axes.stairs(
            signal_db,
            positions,
            baseline=lowest,
            fill=True,
            color=SIGNAL_COLOR,
            alpha=0.35,
            label="Signal",
        )
        axes.fill_between(
            positions,
            np.append(threshold_db, threshold_db[-1]),
            np.maximum(np.append(noise_db, noise_db[-1]), np.append(threshold_db, threshold_db[-1])),
            step="post",
            color=ABOVE_THRESHOLD_COLOR,
            alpha=0.22,
            linewidth=0,
            label="Noise above the threshold",
        )
        axes.stairs(
            threshold_db, positions, baseline=None, color=INK, linewidth=2, label="Masking threshold"
        )
        axes.stairs(
            noise_db, positions, baseline=None, color=NOISE_COLOR, linewidth=2, label="Coding noise"
        )
        axes.set_ylim(lowest, highest)
        _band_ticks(axes, edges)
        axes.set_title(
            f"{CODEC_NAMES[panel.name]}\nMean excess {panel.mean_excess_db:.2f} dB",
            loc="left",
            fontsize=10,
            color=INK,
        )

    all_axes[0].set_ylabel("Level per band in dB SPL", color=INK_SECONDARY, fontsize=9)
    handles, labels = all_axes[0].get_legend_handles_labels()
    figure.legend(
        handles,
        labels,
        loc="lower left",
        bbox_to_anchor=(0.012, 0.0),
        ncol=4,
        frameon=False,
        fontsize=9,
        labelcolor=INK_SECONDARY,
    )
    top = headline(
        figure,
        f"Where the noise goes: {item} at {bitrate // 1000} kbit/s",
        "Mean energy of all frames, measured from the decoded file. Noise below the black line "
        "is masked according to the model.",
    )
    figure.tight_layout(rect=(0, 0.07, 1, top))
    figure.savefig(path, dpi=150, facecolor=SURFACE)
    plt.close(figure)


def plot_measures(path: Path, results: list[dict], synthetic: bool = True) -> None:
    """Three measures, one column per item, to show how they judge the same two encoders."""
    items = list(dict.fromkeys(row["item"] for row in results))
    measures = (
        ("snr_db", "SNR in dB"),
        ("segmental_snr_db", "Segmental SNR in dB"),
        ("mean_excess_db", "Mean excess in dB"),
    )
    figure, all_axes = plt.subplots(
        len(measures),
        len(items),
        figsize=(1.95 * len(items) + 0.9, 7.6),
        sharex=True,
        facecolor=SURFACE,
        squeeze=False,
    )
    for column, item in enumerate(items):
        for row_index, (key, label) in enumerate(measures):
            axes = all_axes[row_index][column]
            style_axes(axes)
            for codec in OWN_CODECS:
                rows = sorted(
                    (r for r in results if r["item"] == item and r["codec"] == codec),
                    key=lambda r: r["bitrate"],
                )
                axes.plot(
                    [r["actual_bitrate"] / 1000 for r in rows],
                    [r[key] for r in rows],
                    color=CODEC_COLORS[codec],
                    linewidth=2,
                    marker=CODEC_MARKERS[codec],
                    markersize=5.5,
                    markeredgecolor=SURFACE,
                    markeredgewidth=1.2,
                    label=CODEC_NAMES[codec],
                )
            if column == 0:
                axes.set_ylabel(label, color=INK_SECONDARY, fontsize=9)
            if row_index == 0:
                axes.set_title(item.replace("_", " "), loc="left", fontsize=10, color=INK)
            if row_index == len(measures) - 1:
                axes.set_xlabel("kbit/s", color=INK_SECONDARY, fontsize=9)
                # One scale, since the excess means the same on all signals.
                if column > 0:
                    axes.sharey(all_axes[row_index][0])
                    axes.tick_params(labelleft=False)
    handles, labels = all_axes[0][0].get_legend_handles_labels()
    figure.legend(
        handles,
        labels,
        loc="lower left",
        bbox_to_anchor=(0.012, 0.0),
        ncol=len(labels),
        frameon=False,
        fontsize=9,
        labelcolor=INK_SECONDARY,
    )
    top = headline(
        figure,
        "The same two encoders by three measures",
        f"Each column is {'one synthetic signal' if synthetic else 'one excerpt'}. Higher is better "
        "for SNR, lower for the excess, which favors the model by construction. The SNR rows use "
        "a scale per column.",
    )
    figure.tight_layout(rect=(0, 0.04, 1, top))
    figure.savefig(path, dpi=150, facecolor=SURFACE)
    plt.close(figure)


def plot_visqol(path: Path, results: list[dict], synthetic: bool = True) -> None:
    """The score of ViSQOL over the real bitrate, one panel per item, on one common scale."""
    items = list(dict.fromkeys(row["item"] for row in results))
    codecs = [codec for codec in CODEC_COLORS if any(r["codec"] == codec for r in results)]
    figure, all_axes = plt.subplots(
        1,
        len(items),
        figsize=(1.95 * len(items) + 0.9, 3.9),
        sharey=True,
        facecolor=SURFACE,
        squeeze=False,
    )
    for axes, item in zip(all_axes[0], items):
        style_axes(axes)
        for codec in codecs:
            rows = sorted(
                (r for r in results if r["item"] == item and r["codec"] == codec),
                key=lambda r: r["bitrate"],
            )
            axes.plot(
                [r["actual_bitrate"] / 1000 for r in rows],
                [r["visqol_mos"] for r in rows],
                color=CODEC_COLORS[codec],
                linewidth=2,
                marker=CODEC_MARKERS[codec],
                markersize=5.5,
                markeredgecolor=SURFACE,
                markeredgewidth=1.2,
                label=CODEC_NAMES[codec],
            )
        axes.set_title(item.replace("_", " "), loc="left", fontsize=10, color=INK)
        axes.set_xlabel("kbit/s", color=INK_SECONDARY, fontsize=9)
    all_axes[0][0].set_ylabel("ViSQOL MOS-LQO", color=INK_SECONDARY, fontsize=9)
    handles, labels = all_axes[0][0].get_legend_handles_labels()
    figure.legend(
        handles,
        labels,
        loc="lower left",
        bbox_to_anchor=(0.012, 0.0),
        ncol=len(labels),
        frameon=False,
        fontsize=9,
        labelcolor=INK_SECONDARY,
    )
    top = headline(
        figure,
        "ViSQOL: one model for all codecs",
        "Higher is better, at most about 4.75. "
        + (
            "Each panel is one synthetic signal. ViSQOL was trained on real music, so these signals "
            "may lie outside of what it knows."
            if synthetic
            else "Each panel is one excerpt of a real recording, mixed down to mono."
        ),
    )
    figure.tight_layout(rect=(0, 0.09, 1, top))
    figure.savefig(path, dpi=150, facecolor=SURFACE)
    plt.close(figure)


def plot_pre_echo(
    path: Path, item: str, bitrate: int, original: np.ndarray, decoded: np.ndarray, attack: int
) -> None:
    """The decoded signal around one burst, to show the noise that comes before it."""
    before = round(0.045 * metrics.SAMPLE_RATE)
    after = round(0.015 * metrics.SAMPLE_RATE)
    span = np.arange(attack - before, attack + after)
    milliseconds = (span - attack) / metrics.SAMPLE_RATE * 1000
    figure, axes = plt.subplots(figsize=(9, 4.2), facecolor=SURFACE)
    style_axes(axes)
    axes.plot(
        milliseconds,
        decoded[span],
        color=CODEC_COLORS["model"],
        linewidth=1.2,
        label=f"Decoded, {CODEC_NAMES['model'].lower()}",
    )
    axes.plot(milliseconds, original[span], color=INK, linewidth=1.2, label="Original")
    axes.set_xlabel("Time relative to the attack in ms", color=INK_SECONDARY, fontsize=9)
    axes.set_ylabel("Amplitude", color=INK_SECONDARY, fontsize=9)
    axes.set_xlim(milliseconds[0], milliseconds[-1])
    limit = 1.1 * float(np.max(np.abs(original[span])))
    axes.set_ylim(-limit, limit)
    early = milliseconds < -1
    axes.annotate(
        "Noise before the attack",
        xy=(-12, float(np.max(np.abs(decoded[span][early])))),
        xytext=(-12, 0.32 * limit),
        ha="center",
        fontsize=9,
        color=INK_SECONDARY,
        arrowprops={"arrowstyle": "-", "color": INK_SECONDARY, "linewidth": 1},
    )
    axes.legend(loc="upper left", frameon=False, fontsize=9, labelcolor=INK_SECONDARY)
    top = headline(
        figure,
        f"Pre-echo: {item.replace('_', ' ')} at {bitrate // 1000} kbit/s",
        "A frame spreads its noise over its whole 43 ms window. Without block switching, it "
        "reaches the quiet part before the attack.",
    )
    figure.tight_layout(rect=(0, 0, 1, top))
    figure.savefig(path, dpi=150, facecolor=SURFACE)
    plt.close(figure)
