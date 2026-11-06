"""Tests of the evaluation. Start them from the root of the repository:

CODEC_TOOLS_DIR=build/default python -m unittest discover --start-directory eval

The class WithRealTools runs the built programs and fails if it is not told where they are, so that
a run in CI cannot pass by leaving them out.
"""
from __future__ import annotations

import csv
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np

import evaluate
import metrics
import signals


def delay_samples(reference: np.ndarray, decoded: np.ndarray, largest: int = 4096) -> int:
    """By how many samples decoded lags behind reference, from their cross-correlation."""
    size = len(reference) + len(decoded)
    spectrum = np.fft.rfft(decoded, size) * np.conj(np.fft.rfft(reference, size))
    correlation = np.fft.irfft(spectrum, size)
    lags = np.concatenate([np.arange(0, largest + 1), np.arange(-largest, 0)])
    return int(lags[np.argmax(np.abs(correlation[lags]))])


class Measures(unittest.TestCase):
    def setUp(self):
        self.rng = np.random.default_rng(5)

    def test_snr_of_a_known_error(self):
        signal = self.rng.standard_normal(1000)
        self.assertAlmostEqual(metrics.snr_db(signal, 0.9 * signal), 20.0, places=9)

    def test_snr_without_error_is_infinite(self):
        signal = self.rng.standard_normal(1000)
        self.assertEqual(metrics.snr_db(signal, signal), float("inf"))

    def test_segmental_snr_of_a_known_error(self):
        signal = 0.5 * self.rng.standard_normal(960 * 5)
        self.assertAlmostEqual(metrics.segmental_snr_db(signal, 0.9 * signal), 20.0, places=9)

    def test_segmental_snr_is_limited_on_both_sides(self):
        signal = 0.5 * self.rng.standard_normal(960 * 5)
        self.assertEqual(metrics.segmental_snr_db(signal, signal), 35.0)
        self.assertEqual(metrics.segmental_snr_db(signal, 0.9999 * signal), 35.0)
        self.assertEqual(metrics.segmental_snr_db(signal, -100 * signal), -10.0)

    def test_segments_last_20_ms(self):
        # Only the first half of each segment has an error. Shorter segments would see a perfect part.
        signal = np.full(960 * 4, 0.5)
        decoded = signal.copy()
        for start in range(0, len(signal), 960):
            decoded[start : start + 480] *= 0.9
        expected = 10 * np.log10(0.25 / (0.5 * 0.05**2))
        self.assertAlmostEqual(metrics.segmental_snr_db(signal, decoded), expected, places=9)

    def test_segmental_snr_leaves_out_silence(self):
        loud = 0.5 * self.rng.standard_normal(960)
        signal = np.concatenate([loud, np.zeros(960 * 3)])
        decoded = np.concatenate([0.9 * loud, 0.1 * self.rng.standard_normal(960 * 3)])
        self.assertAlmostEqual(metrics.segmental_snr_db(signal, decoded), 20.0, places=9)

    def test_segmental_snr_weighs_quiet_and_loud_parts_equally(self):
        loud = 0.5 * self.rng.standard_normal(960)
        quiet = 0.005 * self.rng.standard_normal(960)
        signal = np.concatenate([loud, quiet])
        decoded = np.concatenate([loud, 0.0 * quiet])
        # All of the error is in the quiet part, which the overall SNR hardly notices.
        self.assertGreater(metrics.snr_db(signal, decoded), 35)
        self.assertAlmostEqual(metrics.segmental_snr_db(signal, decoded), 35.0 / 2, places=9)

    def test_mdct_follows_its_definition(self):
        size = metrics.FRAME_LENGTH
        samples = self.rng.standard_normal(size)
        frame = metrics.mdct_frames(samples)[0]
        block = np.concatenate([np.zeros(size), samples])
        n = np.arange(2 * size)
        window = np.sin(np.pi * (n + 0.5) / (2 * size))
        for k in (0, 1, 100, 1023):
            basis = np.cos(np.pi / size * (n + 0.5 + size / 2) * (k + 0.5))
            expected = np.sqrt(2 / size) * np.sum(block * window * basis)
            self.assertAlmostEqual(frame[k], expected, places=9)

    def test_mdct_keeps_the_energy(self):
        samples = self.rng.standard_normal(10 * metrics.FRAME_LENGTH + 17)
        energy = np.sum(metrics.mdct_frames(samples) ** 2)
        self.assertAlmostEqual(energy / np.sum(samples**2), 1.0, places=9)

    def test_mdct_has_the_frame_count_of_the_codec(self):
        for size, expected in ((0, 1), (1, 2), (1024, 2), (1025, 3)):
            self.assertEqual(len(metrics.mdct_frames(np.zeros(size))), expected)

    def test_band_energies_add_up_per_band(self):
        coefficients = np.array([[1.0, 2.0, 3.0, 4.0], [0.0, 0.0, 0.0, 1.0]])
        result = metrics.band_energies(coefficients, np.array([0, 1, 4]))
        np.testing.assert_array_equal(result, [[1.0, 29.0], [0.0, 1.0]])

    def test_excess_counts_only_what_is_above_the_threshold(self):
        result = metrics.excess_db(np.array([1.0, 10.0, 100.0, 0.0]), np.full(4, 10.0))
        np.testing.assert_allclose(result, [0.0, 0.0, 10.0, 0.0])

    def test_level_follows_the_scale_of_the_model(self):
        self.assertAlmostEqual(float(metrics.level_db_spl(np.array(512.0))), 96.0)
        self.assertAlmostEqual(float(metrics.level_db_spl(np.array(5.12))), 76.0)
        self.assertTrue(np.isfinite(metrics.level_db_spl(np.array(0.0))))

    def test_full_scale_sine_has_the_energy_of_the_model(self):
        t = np.arange(20 * metrics.FRAME_LENGTH)
        sine = np.sin(2 * np.pi * 1000 * t / metrics.SAMPLE_RATE)
        # The energy of a frame depends on the phase, which at 1 kHz repeats every three frames.
        energies = np.sum(metrics.mdct_frames(sine)[3:18] ** 2, axis=1)
        self.assertAlmostEqual(float(np.mean(energies)), metrics.FULL_SCALE_ENERGY, delta=0.01)

    def test_table_of_the_analysis_is_read_by_frame_and_band(self):
        header = (
            "frame,band,first_coefficient,last_coefficient,energy,masking_threshold,"
            "quiet_threshold,noise,payload_bytes,budget_bits\n"
        )
        lines = [
            "0,0,0,3,1,2,9,3,10,20\n",
            "0,1,4,9,4,5,9,6,10,20\n",
            "1,0,0,3,7,8,9,9,11,20\n",
            "1,1,4,9,10,11,9,12,11,20\n",
        ]
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "table.csv"
            path.write_text(header + "".join(lines), encoding="ascii")
            analysis = metrics.read_analysis(path)
        np.testing.assert_array_equal(analysis.edges, [0, 4, 10])
        np.testing.assert_array_equal(analysis.energy, [[1, 4], [7, 10]])
        np.testing.assert_array_equal(analysis.masking_threshold, [[2, 5], [8, 11]])
        np.testing.assert_array_equal(analysis.noise, [[3, 6], [9, 12]])

    def test_delay_is_found_in_both_directions(self):
        signal = self.rng.standard_normal(5000)
        self.assertEqual(delay_samples(signal, np.roll(signal, 100)), 100)
        self.assertEqual(delay_samples(signal, np.roll(signal, -37)), -37)
        self.assertEqual(delay_samples(signal, signal), 0)


class Signals(unittest.TestCase):
    def test_every_item_has_the_common_format(self):
        with tempfile.TemporaryDirectory() as folder:
            for name in signals.ITEMS:
                with self.subTest(name):
                    path = Path(folder) / f"{name}.wav"
                    signals.write_item(path, name, duration=1.0)
                    samples = metrics.read_wav(path)
                    self.assertEqual(len(samples), metrics.SAMPLE_RATE)
                    self.assertAlmostEqual(np.max(np.abs(samples)), signals.PEAK, delta=0.01)
                    self.assertEqual(samples[0], 0.0)

    def test_items_are_reproducible(self):
        for name, make in signals.ITEMS.items():
            with self.subTest(name):
                np.testing.assert_array_equal(make(0.5), make(0.5))

    def test_clicks_have_attacks_that_can_be_found(self):
        samples = signals.as_pcm(signals.clicks(2.0)) / 32768
        attack = evaluate.find_attack(samples)
        before = samples[attack - 960 : attack - 48]
        after = samples[attack : attack + 48]
        self.assertLess(np.max(np.abs(before)), 0.01)
        self.assertGreater(np.max(np.abs(after)), 0.1)


@unittest.skipIf(os.name == "nt", "the stand-in for ViSQOL is a shell script")
class ReadingVisqol(unittest.TestCase):
    """With a stand-in, since CI does not build ViSQOL. It says what it was asked and where it ran."""

    def stand_in(self, folder: Path, output: str) -> None:
        binary = folder / "bazel-bin" / "visqol"
        binary.parent.mkdir()
        binary.write_text(
            f'#!/bin/sh\n[ -d model ] || exit 3\necho "{output}"\nprintf "%s\\n" "$@" > arguments.txt\n',
            encoding="ascii",
        )
        binary.chmod(0o755)
        (folder / "model").mkdir()

    def test_the_score_is_read_from_the_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            self.stand_in(folder, "Audio mode\\nMOS-LQO:\\t\\t3.25")
            self.assertEqual(metrics.visqol_mos(folder, Path("a.wav"), Path("b.wav")), 3.25)
            arguments = (folder / "arguments.txt").read_text(encoding="ascii").splitlines()
        self.assertEqual(arguments[0], "--reference_file")
        self.assertEqual(Path(arguments[1]).name, "a.wav")
        self.assertEqual(arguments[2], "--degraded_file")
        self.assertEqual(Path(arguments[3]).name, "b.wav")
        self.assertTrue(Path(arguments[1]).is_absolute())
        self.assertTrue(Path(arguments[3]).is_absolute())

    def test_output_without_a_score_is_an_error(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            self.stand_in(folder, "something else")
            with self.assertRaises(ValueError):
                metrics.visqol_mos(folder, Path("a.wav"), Path("b.wav"))

    def test_a_failing_program_is_an_error(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            self.stand_in(folder, "MOS-LQO: 3.25")
            (folder / "model").rmdir()
            with self.assertRaises(subprocess.CalledProcessError):
                metrics.visqol_mos(folder, Path("a.wav"), Path("b.wav"))

    @unittest.skipUnless(os.environ.get("CODEC_VISQOL_DIR"), "needs a real ViSQOL")
    def test_the_real_program_reaches_its_expected_score(self):
        folder = Path(os.environ["CODEC_VISQOL_DIR"])
        data = folder / "testdata" / "conformance_testdata_subset"
        score = metrics.visqol_mos(
            folder,
            data / "contrabassoon48_stereo.wav",
            data / "contrabassoon48_stereo_24kbps_aac.wav",
        )
        self.assertAlmostEqual(score, 2.346868, places=4)


class WithRealTools(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        folder = os.environ.get("CODEC_TOOLS_DIR")
        if not folder:
            raise RuntimeError("Name the folder of the tools with the variable CODEC_TOOLS_DIR.")
        cls.tools = Path(folder).resolve()
        cls.temporary = tempfile.TemporaryDirectory()
        cls.work = Path(cls.temporary.name)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def item(self, name: str, duration: float = 2.0) -> tuple[Path, np.ndarray]:
        path = self.work / f"{name}_{duration}.wav"
        signals.write_item(path, name, duration)
        return path, metrics.read_wav(path)

    def analyze(self, item: Path, bitrate: int, psy: str) -> metrics.Analysis:
        table = self.work / "analysis.csv"
        evaluate.run(
            evaluate.tool(self.tools, "codec-analyze"),
            item,
            table,
            "--bitrate",
            bitrate,
            "--psy",
            psy,
        )
        return metrics.read_analysis(table)

    def test_mdct_here_equals_the_mdct_of_the_codec(self):
        path, samples = self.item("pink_noise")
        analysis = self.analyze(path, 64000, "on")
        energy = metrics.band_energies(metrics.mdct_frames(samples), analysis.edges)
        np.testing.assert_allclose(energy, analysis.energy, rtol=1e-9, atol=1e-12)

    def test_noise_of_the_decoded_file_equals_the_noise_of_the_encoder(self):
        # An orthonormal lapped transform gives back exactly the error of the coefficients. What is
        # left is the rounding to 16 bits, and the two ends, which the file does not fully contain.
        path, samples = self.item("pink_noise")
        rounding = (1 / 32768) ** 2 / 12
        for bitrate in (32000, 96000):
            for psy in ("on", "off"):
                with self.subTest(bitrate=bitrate, psy=psy):
                    analysis = self.analyze(path, bitrate, psy)
                    decoded_path, _ = evaluate.code_own(self.tools, path, self.work, bitrate, psy)
                    decoded = metrics.read_wav(decoded_path)
                    noise = metrics.band_energies(
                        metrics.mdct_frames(samples - decoded), analysis.edges
                    )
                    widths = np.diff(analysis.edges)
                    allowed = 0.02 * analysis.noise + 50 * widths * rounding
                    difference = np.abs(noise - analysis.noise)
                    self.assertTrue(np.all(difference[2:-2] <= allowed[2:-2]))

    def test_own_decoder_keeps_the_timing(self):
        path, samples = self.item("clicks")
        for psy in ("on", "off"):
            with self.subTest(psy):
                decoded_path, _ = evaluate.code_own(self.tools, path, self.work, 64000, psy)
                self.assertEqual(delay_samples(samples, metrics.read_wav(decoded_path)), 0)

    @unittest.skipUnless(shutil.which("ffmpeg"), "ffmpeg is not installed")
    def test_foreign_decoders_keep_the_timing(self):
        # Without this, a shift of the decoded file would look like a bad codec.
        path, samples = self.item("clicks")
        for codec in evaluate.FOREIGN_CODECS:
            for bitrate in (32000, 96000):
                with self.subTest(codec=codec, bitrate=bitrate):
                    decoded_path, _ = evaluate.code_foreign(codec, path, self.work, bitrate)
                    decoded = evaluate.fitted_to(metrics.read_wav(decoded_path), len(samples))
                    self.assertEqual(delay_samples(samples, decoded), 0)

    def test_model_moves_noise_as_intended(self):
        path, samples = self.item("pink_noise")
        analysis = self.analyze(path, 64000, "on")
        measured = {}
        for psy in ("on", "off"):
            decoded_path, _ = evaluate.code_own(self.tools, path, self.work, 64000, psy)
            decoded = metrics.read_wav(decoded_path)
            measured[psy], _ = evaluate.measure(samples, decoded, analysis)
        self.assertLess(measured["on"]["mean_excess_db"], 0.75 * measured["off"]["mean_excess_db"])
        self.assertGreater(measured["off"]["snr_db"], measured["on"]["snr_db"] + 1)

    @unittest.skipIf(os.name == "nt", "the stand-in for ViSQOL is a shell script")
    def test_whole_evaluation_uses_visqol_if_named(self):
        out = self.work / "out_visqol"
        stand_in = self.work / "visqol"
        (stand_in / "bazel-bin").mkdir(parents=True)
        binary = stand_in / "bazel-bin" / "visqol"
        # The score tells apart what was rated: the bitrate is not in the file name of the original.
        binary.write_text(
            '#!/bin/sh\ncase "$4" in *_model_*) echo "MOS-LQO: 4.5";; *) echo "MOS-LQO: 2.5";; esac\n',
            encoding="ascii",
        )
        binary.chmod(0o755)
        script = Path(__file__).with_name("evaluate.py")
        subprocess.run(
            [
                sys.executable,
                str(script),
                "--tools",
                str(self.tools),
                "--out",
                str(out),
                "--duration",
                "1",
                "--bitrates",
                "48000",
                "--visqol",
                str(stand_in),
            ],
            check=True,
            capture_output=True,
        )
        with open(out / "results.csv", newline="", encoding="ascii") as file:
            rows = list(csv.DictReader(file))
        for row in rows:
            expected = "4.5" if row["codec"] == "model" else "2.5"
            self.assertEqual(row["visqol_mos"], expected, row["codec"])
        self.assertIn("ViSQOL MOS-LQO", (out / "results.md").read_text(encoding="ascii"))
        self.assertGreater((out / "visqol.png").stat().st_size, 0)

    def test_whole_evaluation_takes_own_items(self):
        folder = self.work / "own_items"
        folder.mkdir()
        signals.write_item(folder / "first.wav", "pink_noise", 1.0)
        signals.write_item(folder / "second.wav", "harmonic", 1.0)
        out = self.work / "out_own"
        subprocess.run(
            [
                sys.executable,
                str(Path(__file__).with_name("evaluate.py")),
                "--tools",
                str(self.tools),
                "--out",
                str(out),
                "--items",
                str(folder),
                "--bitrates",
                "48000",
            ],
            check=True,
            capture_output=True,
        )
        with open(out / "results.csv", newline="", encoding="ascii") as file:
            items = {row["item"] for row in csv.DictReader(file)}
        self.assertEqual(items, {"first", "second"})
        report = (out / "results.md").read_text(encoding="ascii")
        self.assertIn(evaluate.OWN_ITEMS_NOTE, report)
        self.assertNotIn(evaluate.SYNTHETIC_NOTE, report)
        self.assertGreater((out / "masking_first_48k.png").stat().st_size, 0)
        self.assertGreater((out / "measures.png").stat().st_size, 0)
        self.assertFalse((out / "pre_echo_clicks_48k.png").exists())

    def test_whole_evaluation_writes_all_results(self):
        out = self.work / "out"
        script = Path(__file__).with_name("evaluate.py")
        subprocess.run(
            [
                sys.executable,
                str(script),
                "--tools",
                str(self.tools),
                "--out",
                str(out),
                "--duration",
                "1",
                "--bitrates",
                "48000",
                "64000",
            ],
            check=True,
            capture_output=True,
        )
        with open(out / "results.csv", newline="", encoding="ascii") as file:
            rows = list(csv.DictReader(file))
        codecs = 2 + (len(evaluate.FOREIGN_CODECS) if shutil.which("ffmpeg") else 0)
        self.assertEqual(len(rows), len(signals.ITEMS) * codecs * 2)

        # The names must fit the encoders behind them. On white noise, the difference is clear.
        def value(codec: str, measure: str) -> float:
            row = next(
                r
                for r in rows
                if (r["item"], r["codec"], r["bitrate"]) == ("white_noise", codec, "48000")
            )
            return float(row[measure])

        self.assertLess(value("model", "mean_excess_db"), value("baseline", "mean_excess_db") / 2)
        self.assertGreater(value("baseline", "snr_db"), value("model", "snr_db"))

        self.assertIn(evaluate.SYNTHETIC_NOTE, (out / "results.md").read_text(encoding="ascii"))

        # Without ViSQOL, nothing pretends to come from it.
        self.assertNotIn("visqol_mos", rows[0])
        self.assertFalse((out / "visqol.png").exists())
        self.assertNotIn("ViSQOL MOS-LQO", (out / "results.md").read_text(encoding="ascii"))

        for name in ("results.md", "measures.png", "pre_echo_clicks_48k.png"):
            self.assertGreater((out / name).stat().st_size, 0, name)
        for name in signals.ITEMS:
            self.assertGreater((out / f"masking_{name}_48k.png").stat().st_size, 0, name)


if __name__ == "__main__":
    unittest.main()
