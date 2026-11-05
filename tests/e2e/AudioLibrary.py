"""Helpers for the end-to-end tests. Uses only the Python standard library."""
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
