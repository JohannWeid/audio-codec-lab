"""Synthetic test signals, so that the evaluation runs without any licensed audio.

They are stand-ins and say nothing about real music. Each one exists to stress one thing:

tone_1khz    one spectral line, which needs almost no bits
harmonic     many lines with quiet gaps between them, where flat noise is exposed
chords       notes that start and decay, as a rough model of tonal music
pink_noise   energy over the whole band, falling with the frequency like much of real music
white_noise  the worst case: nothing is predictable and every band is loud
clicks       short bursts in a quiet background, which provoke pre-echo
"""
from __future__ import annotations

import wave
from pathlib import Path

import numpy as np

from metrics import SAMPLE_RATE

DURATION_SECONDS = 8.0
PEAK = 0.5
FADE_SECONDS = 0.01


def _times(duration: float) -> np.ndarray:
    return np.arange(round(duration * SAMPLE_RATE)) / SAMPLE_RATE


def tone_1khz(duration: float) -> np.ndarray:
    return np.sin(2 * np.pi * 1000 * _times(duration))


def harmonic(duration: float) -> np.ndarray:
    t = _times(duration)
    signal = np.zeros_like(t)
    for number in range(1, 41):
        signal += np.sin(2 * np.pi * 220 * number * t) / number
    return signal


def chords(duration: float) -> np.ndarray:
    t = _times(duration)
    # Four chords of three notes each, in Hz. Each note has harmonics and decays.
    progression = [
        (220.00, 277.18, 329.63),
        (293.66, 369.99, 440.00),
        (246.94, 329.63, 392.00),
        (196.00, 246.94, 293.66),
    ]
    seconds_per_chord = 0.75
    signal = np.zeros_like(t)
    for index in range(int(duration / seconds_per_chord) + 1):
        start = index * seconds_per_chord
        since_start = np.clip(t - start, 0, None)
        envelope = np.where(t >= start, np.exp(-2.5 * since_start), 0.0)
        for frequency in progression[index % len(progression)]:
            for number in range(1, 9):
                signal += envelope * np.sin(2 * np.pi * frequency * number * since_start) / number**2
    return signal


def _shaped_noise(duration: float, seed: int, exponent: float) -> np.ndarray:
    rng = np.random.default_rng(seed)
    size = round(duration * SAMPLE_RATE)
    spectrum = np.fft.rfft(rng.standard_normal(size))
    frequencies = np.fft.rfftfreq(size, 1 / SAMPLE_RATE)
    spectrum[1:] *= frequencies[1:] ** (-exponent / 2)
    spectrum[0] = 0
    return np.fft.irfft(spectrum, size)


def pink_noise(duration: float) -> np.ndarray:
    return _shaped_noise(duration, seed=1, exponent=1.0)


def white_noise(duration: float) -> np.ndarray:
    return _shaped_noise(duration, seed=2, exponent=0.0)


def clicks(duration: float) -> np.ndarray:
    rng = np.random.default_rng(3)
    size = round(duration * SAMPLE_RATE)
    signal = 0.002 * rng.standard_normal(size)
    # Irregular distances, so that the bursts do not fall on the same place of a frame each time.
    position = round(0.25 * SAMPLE_RATE)
    burst_size = round(0.02 * SAMPLE_RATE)
    decay = np.exp(-np.arange(burst_size) / (0.003 * SAMPLE_RATE))
    while position + burst_size < size:
        signal[position : position + burst_size] += decay * rng.standard_normal(burst_size)
        position += round(rng.uniform(0.2, 0.5) * SAMPLE_RATE)
    return signal


ITEMS = {
    "tone_1khz": tone_1khz,
    "harmonic": harmonic,
    "chords": chords,
    "pink_noise": pink_noise,
    "white_noise": white_noise,
    "clicks": clicks,
}


def as_pcm(signal: np.ndarray) -> np.ndarray:
    """Scales to the common peak level and fades the ends, whose jumps would be broadband noise."""
    scaled = PEAK * signal / np.max(np.abs(signal))
    fade = min(round(FADE_SECONDS * SAMPLE_RATE), len(scaled) // 2)
    ramp = np.linspace(0, 1, fade, endpoint=False)
    scaled[:fade] *= ramp
    scaled[len(scaled) - fade :] *= ramp[::-1]
    return np.round(scaled * 32767).astype(np.int16)


def write_item(path: Path, name: str, duration: float = DURATION_SECONDS) -> None:
    samples = as_pcm(ITEMS[name](duration))
    with wave.open(str(path), "wb") as file:
        file.setnchannels(1)
        file.setsampwidth(2)
        file.setframerate(SAMPLE_RATE)
        file.writeframes(samples.astype("<i2").tobytes())
