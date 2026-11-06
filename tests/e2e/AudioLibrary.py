"""Helpers for the end-to-end tests. Uses only the Python standard library."""
import csv
import math
import os
import random
import wave


def tool_path(folder: str, name: str) -> str:
    """Returns the absolute path of a tool, so that the tests can change directories safely."""
    return os.path.abspath(os.path.join(folder, name))


def _signal_value(kind, n, sample_rate, rng):
    t = n / sample_rate
    if kind == "silence":
        return 0.0
    if kind == "tone":
        return 0.5 * math.sin(2 * math.pi * 1000 * t)
    if kind == "mixture":
        return (
            0.3 * math.sin(2 * math.pi * 220 * t)
            + 0.15 * math.sin(2 * math.pi * 1250 * t)
            + 0.05 * math.sin(2 * math.pi * 5100 * t)
            + 0.02 * rng.uniform(-1, 1)
        )
    if kind == "square":
        return 1.0 if (n // 37) % 2 == 0 else -1.0
    raise ValueError(f"unknown signal: {kind}")


def write_test_signal(
    path: str,
    kind: str,
    samples: int,
    sample_rate: int = 48000,
    channels: int = 1,
    sample_width: int = 2,
):
    """Writes a WAV file. The sample count is per channel."""
    rng = random.Random(1)
    limit = 2 ** (8 * sample_width - 1) - 1
    data = bytearray()
    for n in range(samples):
        value = round(limit * _signal_value(kind, n, sample_rate, rng))
        for _ in range(channels):
            data += value.to_bytes(sample_width, "little", signed=True)
    with wave.open(path, "wb") as file:
        file.setnchannels(channels)
        file.setsampwidth(sample_width)
        file.setframerate(sample_rate)
        file.writeframes(bytes(data))


def _read_samples(path):
    with wave.open(path, "rb") as file:
        data = file.readframes(file.getnframes())
    return [int.from_bytes(data[i : i + 2], "little", signed=True) for i in range(0, len(data), 2)]


def wav_should_have_format(path: str, samples: int, sample_rate: int = 48000, channels: int = 1):
    with wave.open(path, "rb") as file:
        actual = (file.getframerate(), file.getnchannels(), file.getsampwidth(), file.getnframes())
    expected = (sample_rate, channels, 2, samples)
    if actual != expected:
        raise AssertionError(
            f"rate, channels, bytes per sample, length: expected {expected}, found {actual}"
        )


def snr_db(reference: str, decoded: str) -> float:
    """SNR of a decoded file relative to the original, which also fails if they are misaligned."""
    a = _read_samples(reference)
    b = _read_samples(decoded)
    if len(a) != len(b):
        raise AssertionError(f"the files have {len(a)} and {len(b)} samples")
    signal = sum(x * x for x in a)
    error = sum((x - y) ** 2 for x, y in zip(a, b))
    return math.inf if error == 0 else 10 * math.log10(signal / error)


def wav_should_be_silent(path: str):
    if any(_read_samples(path)):
        raise AssertionError(f"{path} is not silent")


def analysis_should_describe_stream(analysis: str, stream: str, samples: int):
    """The table must have one line per frame and band, and agree with the size of the bitstream."""
    with open(analysis, newline="", encoding="ascii") as file:
        rows = list(csv.DictReader(file))
    frames = math.ceil(samples / 1024) + 1
    if len(rows) != frames * 25:
        raise AssertionError(f"expected {frames * 25} lines, found {len(rows)}")
    for row in rows:
        for name in ("energy", "masking_threshold", "quiet_threshold", "noise"):
            if not math.isfinite(float(row[name])) or float(row[name]) < 0:
                raise AssertionError(f"{name} is not a finite energy in {row}")
    if rows[-1]["last_coefficient"] != "1023":
        raise AssertionError(f"the bands must end at coefficient 1023: {rows[-1]}")
    payloads = sum(int(row["payload_bytes"]) for row in rows if row["band"] == "0")
    expected = 20 + 2 * frames + payloads
    actual = os.path.getsize(stream)
    if actual != expected:
        raise AssertionError(f"the table says {expected} bytes, the bitstream has {actual}")


def mean_excess_db(analysis: str) -> float:
    """Mean over all bands of how far the noise is above the threshold. Bands below it count as 0."""
    with open(analysis, newline="", encoding="ascii") as file:
        rows = list(csv.DictReader(file))
    total = 0.0
    for row in rows:
        noise = float(row["noise"])
        threshold = float(row["masking_threshold"])
        if noise > threshold:
            total += 10 * math.log10(noise / threshold)
    return total / len(rows)


def largest_allowed_stream_size(samples: int, bitrate: int) -> int:
    """Header plus the budget of all frames, from section 6 of the specification, in bytes."""
    frames = math.ceil(samples / 1024) + 1
    return 20 + frames * bitrate * 1024 // 48000 // 8


def copy_truncated(source: str, target: str, size: int):
    with open(source, "rb") as file:
        data = file.read(size)
    with open(target, "wb") as file:
        file.write(data)


def copy_with_damaged_byte(source: str, target: str, position: int):
    with open(source, "rb") as file:
        data = bytearray(file.read())
    data[position] ^= 0xFF
    with open(target, "wb") as file:
        file.write(data)
