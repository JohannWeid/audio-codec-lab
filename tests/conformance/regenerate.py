#!/usr/bin/env python3
"""Creates the reference files in this directory, see README.md.

Usage: regenerate.py FOLDER_OF_THE_TOOLS

Run this deliberately only: the point of these files is that they do not change by accident.
"""
import math
import pathlib
import random
import subprocess
import sys
import wave

SAMPLE_RATE = 48000
SAMPLE_COUNT = 12000
# Name of the reference: bitrate, and whether the psychoacoustic model is on.
REFERENCES = {
    "ref_32k": (32000, "off"),
    "ref_64k": (64000, "off"),
    "ref_psy_64k": (64000, "on"),
}
DIRECTORY = pathlib.Path(__file__).resolve().parent


def make_signal():
    """Tones with a changing level, a little noise and short clicks, to reach all parts of the codec."""
    rng = random.Random(20261105)
    samples = []
    for n in range(SAMPLE_COUNT):
        t = n / SAMPLE_RATE
        level = 0.5 + 0.5 * math.sin(2 * math.pi * 8 * t)
        value = (
            0.35 * level * math.sin(2 * math.pi * 440 * t)
            + 0.15 * math.sin(2 * math.pi * 1320 * t)
            + 0.05 * math.sin(2 * math.pi * 7000 * t)
            + 0.03 * rng.uniform(-1, 1)
        )
        if n % 4000 == 3000:
            value += 0.5
        samples.append(max(-32768, min(32767, round(value * 32767))))
    return samples


def write_wav(path, samples):
    with wave.open(str(path), "wb") as file:
        file.setnchannels(1)
        file.setsampwidth(2)
        file.setframerate(SAMPLE_RATE)
        file.writeframes(b"".join(s.to_bytes(2, "little", signed=True) for s in samples))


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    tools = pathlib.Path(sys.argv[1]).resolve()

    source = DIRECTORY / "input.wav"
    write_wav(source, make_signal())
    for name, (bitrate, psychoacoustics) in REFERENCES.items():
        stream = DIRECTORY / f"{name}.bin"
        decoded = DIRECTORY / f"{name}.wav"
        subprocess.run(
            [tools / "codec-encode", source, stream, "--bitrate", str(bitrate), "--psy", psychoacoustics],
            check=True,
        )
        subprocess.run([tools / "codec-decode", stream, decoded], check=True)
    print("Now run the conformance tests and update the hashes in tests/test_conformance.cpp.")


if __name__ == "__main__":
    main()
