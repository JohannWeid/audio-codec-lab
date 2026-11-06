#!/usr/bin/env python3
"""Prepares the audio and the configuration of the listening test. See listening-test/README.md."""
from __future__ import annotations

import argparse
import csv
import json
import re
import shutil
import subprocess
import sys
import wave
from dataclasses import dataclass
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "eval"))

import evaluate  # noqa: E402
import metrics  # noqa: E402

SAMPLE_RATE = metrics.SAMPLE_RATE
TEST_ID = "audio_codec_lab"
TARGET_LUFS = -23.0
PEAK_LIMIT = 10 ** (-1 / 20)
FADE_SECONDS = 0.02
LONGEST_EXCERPT_SECONDS = 12.0
FILTER_TAPS = 2047
MOST_SIGNALS_PER_TRIAL = 12
AUDIO_FOLDER = "configs/resources/audio_codec_lab"
TRAINING_CATEGORY = "training"
REFERENCES_FOLDER = "references"
MAGIC_QUOTES_CHECK = re.compile(
    r"if\s*\(\s*get_magic_quotes_gpc\(\)\s*\)\s*\{.*?\}\s*else\s*\{\s*(.*?;)\s*\}", re.DOTALL
)
COUNT_OF_A_VARIABLE = re.compile(r"\bcount\((\$[^()]*)\)")


@dataclass(frozen=True)
class Condition:
    """A signal in a trial. The name is what the results call it."""

    name: str
    kind: str
    bitrate: int = 0


# The anchors are the two of ITU-R BS.1534-3. webMUSHRA adds the hidden reference by itself and calls
# it "reference". The baseline runs at each bitrate of the model, since the question is whether the
# model sounds better at the same bitrate.
CONDITIONS = (
    Condition("anchor35", "lowpass", 3500),
    Condition("anchor70", "lowpass", 7000),
    Condition("model_32k", "model", 32000),
    Condition("baseline_32k", "baseline", 32000),
    Condition("model_64k", "model", 64000),
    Condition("baseline_64k", "baseline", 64000),
    Condition("opus_64k", "opus", 64000),
)

# For the participants, who are expected to speak German.
TEXT = {
    "welcome_name": "Willkommen",
    "welcome": (
        "Dieser Hörtest vergleicht verschieden stark komprimierte Versionen von kurzen "
        "Audioausschnitten. Er dauert etwa 30 bis 40 Minuten, und du kannst zwischendurch Pausen "
        "machen. Bitte nutze gute Kopfhörer in einer ruhigen Umgebung und den Browser Chrome. "
        "Bewertet wird das Gehör, nicht du: Es gibt keine falschen Antworten."
    ),
    "volume_name": "Lautstärke",
    "volume": (
        "Stelle eine angenehme, eher kräftige Lautstärke ein und lasse sie danach bis zum Ende "
        "unverändert."
    ),
    "training_name": "Übung",
    "training": (
        "Dies ist eine Übung, sie wird nicht gewertet. <b>Reference</b> ist das Original. Die "
        "Buttons daneben sind Versionen in zufälliger Reihenfolge, eine davon ist das Original "
        "selbst. Höre alle an und bewerte sie mit den Reglern: 100 heißt kein Unterschied zum "
        "Original. <b>Alle Regler stehen anfangs auf 100, stelle also jeden einzelnen bewusst "
        "ein.</b> Das versteckte Original sollte 100 bekommen."
    ),
    "trial": (
        "Höre jede Version an und stelle jeden Regler bewusst ein, 100 heißt kein Unterschied zu "
        "<b>Reference</b>. Du kannst so oft wechseln, wie du möchtest."
    ),
    "finish_name": "Danke",
    "finish": "Vielen Dank fürs Mitmachen! Bitte schicke die Ergebnisse jetzt ab.",
    "listener": "Kürzel (zum Beispiel zwei Buchstaben, kein voller Name)",
    "headphones": "Kopfhörer (Modell oder Art)",
    "experience": "Erfahrung mit kritischem Hören",
    "experience_levels": ("keine", "etwas", "viel"),
}


@dataclass(frozen=True)
class Item:
    name: str
    file: str
    start: float
    duration: float
    category: str
    source: str
    license: str


def read_items(path: Path) -> list[Item]:
    """The list of excerpts. It says where each one comes from, so that nothing is unaccounted for."""
    with open(path, newline="", encoding="utf-8") as file:
        rows = list(csv.DictReader(file))
    items = []
    for row in rows:
        duration = float(row["duration_seconds"])
        if duration <= 0 or duration > LONGEST_EXCERPT_SECONDS:
            raise ValueError(f"{row['name']}: an excerpt should last up to about 10 s, see BS.1534")
        if not re.fullmatch(r"[A-Za-z0-9_]+", row["name"]):
            raise ValueError(f"{row['name']}: a name may only have letters, digits and underscores")
        if not row["source"].strip() or not row["license"].strip():
            raise ValueError(f"{row['name']}: the source and the license must be known")
        items.append(
            Item(
                row["name"],
                row["file"],
                float(row["start_seconds"]),
                duration,
                row["category"],
                row["source"],
                row["license"],
            )
        )
    names = [item.name for item in items]
    if len(set(names)) != len(names):
        raise ValueError("the names of the items must be different")
    return items


def split_items(items: list[Item]) -> tuple[list[Item], str]:
    """The items of the trials, and the name of the one for the training, which is not rated.

    Without an item of the category "training", the first one is used for both.
    """
    training = [item for item in items if item.category == TRAINING_CATEGORY]
    trials = [item for item in items if item.category != TRAINING_CATEGORY]
    if len(training) > 1:
        raise ValueError("there can only be one item for the training")
    if not trials:
        raise ValueError("there are no items for the trials")
    return trials, training[0].name if training else trials[0].name


def write_wav(path: Path, samples: np.ndarray) -> None:
    """Writes samples in the range of metrics.read_wav as 16-bit mono."""
    values = np.clip(np.round(samples * 32768), -32768, 32767).astype("<i2")
    with wave.open(str(path), "wb") as file:
        file.setnchannels(1)
        file.setsampwidth(2)
        file.setframerate(SAMPLE_RATE)
        file.writeframes(values.tobytes())


def run_ffmpeg(*arguments) -> str:
    command = ["ffmpeg", "-y", "-hide_banner", *[str(a) for a in arguments]]
    result = subprocess.run(command, check=True, capture_output=True, text=True, errors="replace")
    return result.stderr


def cut_to_mono(source: Path, start: float, duration: float, target: Path) -> None:
    run_ffmpeg(
        "-loglevel",
        "error",
        "-ss",
        start,
        "-t",
        duration,
        "-i",
        source,
        "-ac",
        1,
        "-ar",
        SAMPLE_RATE,
        "-c:a",
        "pcm_s16le",
        target,
    )


def loudness_lufs(path: Path) -> float:
    """The integrated loudness of EBU R 128, as measured by ffmpeg."""
    output = run_ffmpeg("-nostats", "-i", path, "-af", "loudnorm=print_format=json", "-f", "null", "-")
    report = json.loads(output[output.rindex("{") : output.rindex("}") + 1])
    return float(report["input_i"])


def normalized(samples: np.ndarray, loudness: float) -> tuple[np.ndarray, float]:
    """Brings an excerpt to the common loudness unless it would exceed the peak limit. Returns dB."""
    gain = 10 ** ((TARGET_LUFS - loudness) / 20)
    peak = float(np.max(np.abs(samples)))
    if peak * gain > PEAK_LIMIT:
        gain = PEAK_LIMIT / peak
    fade = round(FADE_SECONDS * SAMPLE_RATE)
    ramp = np.linspace(0, 1, fade, endpoint=False)
    result = samples * gain
    result[:fade] *= ramp
    result[len(result) - fade :] *= ramp[::-1]
    return result, 20 * float(np.log10(gain))


def lowpass(samples: np.ndarray, cutoff_hz: float) -> np.ndarray:
    """A linear-phase filter with a Blackman window, which keeps the length and the timing."""
    middle = (FILTER_TAPS - 1) // 2
    n = np.arange(FILTER_TAPS) - middle
    taps = 2 * cutoff_hz / SAMPLE_RATE * np.sinc(2 * cutoff_hz / SAMPLE_RATE * n)
    taps *= np.blackman(FILTER_TAPS)
    taps /= np.sum(taps)
    size = len(samples) + FILTER_TAPS - 1
    filtered = np.fft.irfft(np.fft.rfft(samples, size) * np.fft.rfft(taps, size), size)
    return filtered[middle : middle + len(samples)]


def make_condition(
    condition: Condition, tools: Path, reference: Path, work: Path, target: Path
) -> None:
    original = metrics.read_wav(reference)
    if condition.kind == "lowpass":
        write_wav(target, lowpass(original, condition.bitrate))
        return
    if condition.kind in ("model", "baseline"):
        psy = "on" if condition.kind == "model" else "off"
        decoded, _ = evaluate.code_own(tools, reference, work, condition.bitrate, psy)
    else:
        decoded, _ = evaluate.code_foreign(condition.kind, reference, work, condition.bitrate)
    write_wav(target, evaluate.fitted_to(metrics.read_wav(decoded), len(original)))


def _yaml_text(text: str) -> str:
    """Double quotes make YAML take a text literally, also with a colon or a sharp sign in it."""
    return json.dumps(text, ensure_ascii=False)


def mushra_page(page_id: str, name: str, content: str, item: str, conditions) -> list[str]:
    folder = f"{AUDIO_FOLDER}/{item}"
    lines = [
        "type: mushra",
        f"id: {page_id}",
        f"name: {_yaml_text(name)}",
        f"content: {_yaml_text(content)}",
        "showWaveform: true",
        "enableLooping: true",
        "randomize: true",
        "showConditionNames: false",
        "createAnchor35: false",
        "createAnchor70: false",
        f"reference: {folder}/reference.wav",
        "stimuli:",
    ]
    lines += [f"    {c.name}: {folder}/{c.name}.wav" for c in conditions]
    return lines


def _as_list_entry(lines: list[str], indent: int) -> list[str]:
    prefix = " " * indent
    return [f"{prefix}- {lines[0]}"] + [f"{prefix}  {line}" for line in lines[1:]]


def configuration(trials: list[Item], training: str, conditions=CONDITIONS) -> str:
    """The test as webMUSHRA reads it: greeting, volume, training, all trials in random order, end."""
    if len(conditions) + 1 > MOST_SIGNALS_PER_TRIAL:
        raise ValueError("BS.1534 recommends at most 12 signals per trial")
    lines = [
        f"testname: {_yaml_text('Hörtest Audio-Codec')}",
        f"testId: {TEST_ID}",
        "bufferSize: 2048",
        "stopOnErrors: true",
        "showButtonPreviousPage: true",
        "remoteService: service/write.php",
        "",
        "pages:",
    ]
    welcome = [
        "type: generic",
        "id: welcome",
        f"name: {_yaml_text(TEXT['welcome_name'])}",
        f"content: {_yaml_text(TEXT['welcome'])}",
    ]
    volume = [
        "type: volume",
        "id: volume",
        f"name: {_yaml_text(TEXT['volume_name'])}",
        f"content: {_yaml_text(TEXT['volume'])}",
        f"stimulus: {AUDIO_FOLDER}/{training}/reference.wav",
        "defaultVolume: 0.5",
    ]
    lines += _as_list_entry(welcome, 4)
    lines += _as_list_entry(volume, 4)
    lines += _as_list_entry(
        mushra_page("training", TEXT["training_name"], TEXT["training"], training, conditions), 4
    )
    lines += ["    -", "        - random"]
    for item in trials:
        page = mushra_page(f"trial_{item.name}", "Bewertung", TEXT["trial"], item.name, conditions)
        lines += _as_list_entry(page, 8)
    lines += [
        "    - type: finish",
        f"      name: {_yaml_text(TEXT['finish_name'])}",
        f"      content: {_yaml_text(TEXT['finish'])}",
        "      showResults: false",
        "      writeResults: true",
        "      questionnaire:",
        "          - type: text",
        f"            label: {_yaml_text(TEXT['listener'])}",
        "            name: listener",
        "          - type: text",
        f"            label: {_yaml_text(TEXT['headphones'])}",
        "            name: headphones",
        "          - type: likert",
        "            name: experience",
        f"            label: {_yaml_text(TEXT['experience'])}",
        "            response:",
    ]
    for value, label in enumerate(TEXT["experience_levels"]):
        lines += [f"             - value: {value}", f"               label: {_yaml_text(label)}"]
    return "\n".join(lines) + "\n"


def make_stimuli(items: list[Item], tools: Path, source_folder: Path, out: Path) -> list[dict]:
    trials, training = split_items(items)
    work = out / "_work"
    work.mkdir(parents=True, exist_ok=True)
    report = []
    for item in items:
        folder = out / item.name
        folder.mkdir(parents=True, exist_ok=True)
        cut = work / "cut.wav"
        cut_to_mono(source_folder / item.file, item.start, item.duration, cut)
        samples, gain_db = normalized(metrics.read_wav(cut), loudness_lufs(cut))
        reference = folder / "reference.wav"
        write_wav(reference, samples)
        for condition in CONDITIONS:
            make_condition(condition, tools, reference, work, folder / f"{condition.name}.wav")
        report.append({"item": item.name, "gain_db": round(gain_db, 2)})
        print(f"{item.name}: gain {gain_db:+.1f} dB", flush=True)
    shutil.rmtree(work)
    # All originals in one folder, in the form that eval/evaluate.py --items reads.
    references = out / REFERENCES_FOLDER
    references.mkdir(exist_ok=True)
    for item in trials:
        shutil.copy(out / item.name / "reference.wav", references / f"{item.name}.wav")
    (out / "mushra.yaml").write_text(configuration(trials, training), encoding="utf-8")
    return report


def repair_result_service(webmushra: Path) -> None:
    """Makes the service of webMUSHRA 1.4.3 work on PHP 8.

    PHP 8 removed a function that the service calls, so it fails on every call, and the page thanks
    the listener all the same: all ratings are lost without any notice. PHP 8 also no longer accepts
    count(null), which the service reaches for each type of test that a session did not use. Only the
    copy in the given folder changes.
    """
    path = webmushra / "service" / "write.php"
    if not path.exists():
        return
    # The file is not necessarily UTF-8, and Latin-1 brings back every byte as it was.
    text = path.read_bytes().decode("latin-1")
    repaired = MAGIC_QUOTES_CHECK.sub(r"\1", text)
    repaired = COUNT_OF_A_VARIABLE.sub(r"count((array)\1)", repaired)
    if "get_magic_quotes_gpc" in repaired:
        raise ValueError(f"{path} has an unknown form, so it could not be repaired")
    path.write_bytes(repaired.encode("latin-1"))


def install_into_webmushra(out: Path, webmushra: Path, items: list[Item]) -> None:
    repair_result_service(webmushra)
    audio = webmushra / AUDIO_FOLDER
    audio.mkdir(parents=True, exist_ok=True)
    for item in items:
        shutil.copytree(out / item.name, audio / item.name, dirs_exist_ok=True)
    shutil.copy(out / "mushra.yaml", webmushra / "configs" / f"{TEST_ID}.yaml")
    print(f"Installed. The address is http://localhost:8000/?config={TEST_ID}.yaml")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tools", type=Path, required=True, help="folder of the built tools")
    parser.add_argument("--items", type=Path, default=HERE / "items.csv", help="list of excerpts")
    parser.add_argument("--source", type=Path, default=HERE / "items", help="folder of the audio")
    parser.add_argument("--out", type=Path, default=HERE / "stimuli", help="folder for the stimuli")
    parser.add_argument("--webmushra", type=Path, help="copy all into this webMUSHRA folder")
    arguments = parser.parse_args()

    if not shutil.which("ffmpeg"):
        parser.error("ffmpeg is needed for this script")
    items = read_items(arguments.items)
    if not items:
        parser.error(f"{arguments.items} does not list any excerpt yet")

    make_stimuli(items, arguments.tools, arguments.source, arguments.out)
    if arguments.webmushra:
        install_into_webmushra(arguments.out, arguments.webmushra, items)
    print(f"The stimuli are in {arguments.out}")


if __name__ == "__main__":
    main()
