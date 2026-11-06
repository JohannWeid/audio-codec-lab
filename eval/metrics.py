"""Measures that compare a decoded file with its original. Works the same for any codec."""
from __future__ import annotations

import csv
import re
import subprocess
import wave
from dataclasses import dataclass
from pathlib import Path

import numpy as np

SAMPLE_RATE = 48000
FRAME_LENGTH = 1024
FULL_SCALE_DB_SPL = 96.0
FULL_SCALE_ENERGY = 512.0

# Common limits of the segmental SNR, so that silent or perfect segments cannot dominate the mean.
SEGMENT_SAMPLES = 960
SEGMENT_FLOOR_DB = -10.0
SEGMENT_CEILING_DB = 35.0
SEGMENT_SILENCE = 1e-6


def read_wav(path: Path) -> np.ndarray:
    """The samples of a 16-bit mono file as float64, scaled like the codec does it (1/32768)."""
    with wave.open(str(path), "rb") as file:
        if file.getsampwidth() != 2 or file.getnchannels() != 1:
            raise ValueError(f"{path} is not 16-bit mono")
        data = file.readframes(file.getnframes())
    return np.frombuffer(data, dtype="<i2").astype(np.float64) / 32768


def snr_db(reference: np.ndarray, decoded: np.ndarray) -> float:
    error = np.sum((reference - decoded) ** 2)
    if error == 0:
        return float("inf")
    return float(10 * np.log10(np.sum(reference**2) / error))


def segmental_snr_db(reference: np.ndarray, decoded: np.ndarray) -> float:
    """The mean SNR of segments of 20 ms, with silent segments left out and each one limited."""
    count = len(reference) // SEGMENT_SAMPLES
    values = []
    for index in range(count):
        part = slice(index * SEGMENT_SAMPLES, (index + 1) * SEGMENT_SAMPLES)
        signal = np.mean(reference[part] ** 2)
        if signal < SEGMENT_SILENCE:
            continue
        error = np.mean((reference[part] - decoded[part]) ** 2)
        value = SEGMENT_CEILING_DB if error == 0 else 10 * np.log10(signal / error)
        values.append(np.clip(value, SEGMENT_FLOOR_DB, SEGMENT_CEILING_DB))
    return float(np.mean(values)) if values else SEGMENT_CEILING_DB


def mdct_frames(samples: np.ndarray) -> np.ndarray:
    """The same framing and orthonormal MDCT as the codec, as an array of frames by coefficients."""
    size = FRAME_LENGTH
    count = int(np.ceil(len(samples) / size)) + 1
    padded = np.concatenate([np.zeros(size), samples, np.zeros(2 * size)])
    window = np.sin(np.pi * (np.arange(2 * size) + 0.5) / (2 * size))
    blocks = np.stack([padded[i * size : (i + 2) * size] for i in range(count)]) * window

    # cos(pi/M (n + n0)(k + 1/2)) as the real part of an FFT of size 2M with two rotations.
    n0 = 0.5 + size / 2
    before = np.exp(-1j * np.pi * np.arange(2 * size) / (2 * size))
    after = np.exp(-1j * np.pi * n0 * (np.arange(size) + 0.5) / size)
    spectrum = np.fft.fft(blocks * before, axis=1)[:, :size]
    return np.sqrt(2 / size) * np.real(spectrum * after)


def band_energies(coefficients: np.ndarray, edges: np.ndarray) -> np.ndarray:
    """Energy per frame and band. The array edges holds the first coefficient of each band."""
    return np.add.reduceat(coefficients**2, edges[:-1], axis=1)


def excess_db(noise: np.ndarray, thresholds: np.ndarray) -> np.ndarray:
    """How far the noise is above the threshold in each band. Bands below it count as 0."""
    above = noise > thresholds
    result = np.zeros_like(noise)
    result[above] = 10 * np.log10(noise[above] / thresholds[above])
    return result


def level_db_spl(energy: np.ndarray) -> np.ndarray:
    """On the scale of the psychoacoustic model: a full scale sine has the energy 512 and 96 dB SPL."""
    tiny = FULL_SCALE_ENERGY * 1e-20
    return FULL_SCALE_DB_SPL + 10 * np.log10(np.maximum(energy, tiny) / FULL_SCALE_ENERGY)


def visqol_mos(folder: Path, reference: Path, degraded: Path) -> float:
    """The score of ViSQOL in its audio mode. The folder is the one built by build_visqol.sh.

    It looks for its model relative to the working directory, so it has to run from its own folder.
    """
    result = subprocess.run(
        [
            str(Path(folder).resolve() / "bazel-bin" / "visqol"),
            "--reference_file",
            str(Path(reference).resolve()),
            "--degraded_file",
            str(Path(degraded).resolve()),
        ],
        cwd=folder,
        check=True,
        capture_output=True,
        text=True,
    )
    match = re.search(r"MOS-LQO:\s+([0-9.]+)", result.stdout)
    if not match:
        raise ValueError(f"ViSQOL gave no score: {result.stdout}{result.stderr}")
    return float(match.group(1))


@dataclass
class Analysis:
    """The table that codec-analyze writes, as arrays of frames by bands."""

    edges: np.ndarray
    energy: np.ndarray
    masking_threshold: np.ndarray
    noise: np.ndarray


def read_analysis(path: Path) -> Analysis:
    with open(path, newline="", encoding="ascii") as file:
        rows = list(csv.DictReader(file))
    bands = 1 + max(int(row["band"]) for row in rows)
    frames = len(rows) // bands

    def column(name: str) -> np.ndarray:
        return np.array([float(row[name]) for row in rows]).reshape(frames, bands)

    edges = [int(row["first_coefficient"]) for row in rows[:bands]]
    edges.append(int(rows[bands - 1]["last_coefficient"]) + 1)
    return Analysis(
        edges=np.array(edges),
        energy=column("energy"),
        masking_threshold=column("masking_threshold"),
        noise=column("noise"),
    )
