"""Tests of the listening test scripts. Start them from the root of the repository:

CODEC_TOOLS_DIR=build/default python -m unittest discover --start-directory listening-test

All ratings here are made up for the tests. The class WithRealTools runs the built programs and ffmpeg.
"""
from __future__ import annotations

import csv
import http.server
import io
import os
import shutil
import tempfile
import threading
import unittest
import wave
import zipfile
from pathlib import Path

import numpy as np

import analyze
import fetch_items
import make_stimuli
import metrics
import signals


def ratings_of(table: dict) -> list[analyze.Rating]:
    """From {listener: {item: {condition: score}}}."""
    return [
        analyze.Rating(listener, item, condition, float(score))
        for listener, items in table.items()
        for item, scores in items.items()
        for condition, score in scores.items()
    ]


def good_listener(items: list[str]) -> dict:
    return {
        item: {"reference": 100, "anchor70": 60, "anchor35": 20, "model_64k": 80, "baseline_64k": 50}
        for item in items
    }


ITEMS = [f"item{i}" for i in range(8)]


class ReadRatings(unittest.TestCase):
    def write(self, folder: str, rows: list[dict]) -> Path:
        path = Path(folder) / "mushra.csv"
        with open(path, "w", newline="", encoding="utf-8") as file:
            writer = csv.DictWriter(file, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)
        return path

    @staticmethod
    def row(listener: str, trial: str, stimulus: str, score: int) -> dict:
        return {
            "session_test_id": "audio_codec_lab",
            "listener": listener,
            "headphones": "any",
            "experience": "0",
            "trial_id": trial,
            "rating_stimulus": stimulus,
            "rating_score": str(score),
            "rating_time": "120",
            "rating_comment": "",
        }

    def test_training_is_left_out(self):
        with tempfile.TemporaryDirectory() as folder:
            path = self.write(
                folder,
                [self.row("a", "training", "reference", 1), self.row("a", "trial_x", "reference", 90)],
            )
            ratings = analyze.read_ratings(path)
        self.assertEqual(ratings, [analyze.Rating("a", "x", "reference", 90.0)])

    def test_the_session_id_is_preferred_to_the_typed_name(self):
        rows = [
            {**self.row("same", "trial_x", "reference", 90), "session_uuid": "first"},
            {**self.row("same", "trial_x", "reference", 80), "session_uuid": "second"},
        ]
        with tempfile.TemporaryDirectory() as folder:
            ratings = analyze.read_ratings(self.write(folder, rows))
        self.assertEqual([r.listener for r in ratings], ["first", "second"])

    def test_a_name_used_twice_is_rejected(self):
        rows = [self.row("a", "trial_x", "reference", 90), self.row("a", "trial_x", "reference", 80)]
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaises(ValueError):
                analyze.read_ratings(self.write(folder, rows))

    def test_an_unknown_trial_is_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            path = self.write(folder, [self.row("a", "other", "reference", 90)])
            with self.assertRaises(ValueError):
                analyze.read_ratings(path)


class PostScreening(unittest.TestCase):
    def listeners(self, count: int = 8) -> dict:
        return {f"L{i}": good_listener(ITEMS) for i in range(count)}

    def test_good_listeners_stay(self):
        result = analyze.post_screen(ratings_of(self.listeners()))
        self.assertEqual(len(result.kept), 8)
        self.assertEqual(result.excluded, {})

    def test_one_low_reference_in_eight_items_is_allowed(self):
        table = self.listeners()
        table["L0"]["item0"]["reference"] = 50
        self.assertNotIn("L0", analyze.post_screen(ratings_of(table)).excluded)

    def test_two_low_references_in_eight_items_exclude(self):
        table = self.listeners()
        table["L0"]["item0"]["reference"] = 50
        table["L0"]["item1"]["reference"] = 89
        result = analyze.post_screen(ratings_of(table))
        self.assertIn("L0", result.excluded)
        self.assertIn("hidden reference", result.excluded["L0"])

    def test_a_score_of_exactly_90_is_not_lower_than_90(self):
        table = self.listeners()
        for item in ITEMS:
            table["L0"][item]["reference"] = 90
        self.assertNotIn("L0", analyze.post_screen(ratings_of(table)).excluded)

    def test_a_high_mid_anchor_on_two_items_excludes(self):
        table = self.listeners()
        table["L0"]["item0"]["anchor70"] = 95
        table["L0"]["item1"]["anchor70"] = 91
        result = analyze.post_screen(ratings_of(table))
        self.assertIn("L0", result.excluded)
        self.assertIn("mid anchor", result.excluded["L0"])

    def test_a_score_of_exactly_90_is_not_higher_than_90(self):
        table = self.listeners()
        for item in ITEMS:
            table["L0"][item]["anchor70"] = 90
        self.assertNotIn("L0", analyze.post_screen(ratings_of(table)).excluded)

    def test_an_item_with_a_weak_anchor_counts_against_nobody(self):
        # Three of eight listeners, so more than 25 %, rate the anchor high on the same two items.
        table = self.listeners()
        for name in ("L0", "L1", "L2"):
            table[name]["item0"]["anchor70"] = 95
            table[name]["item1"]["anchor70"] = 95
        result = analyze.post_screen(ratings_of(table))
        self.assertEqual(result.excluded, {})
        self.assertEqual(result.uninformative_anchor_items, ["item0", "item1"])

    def test_two_of_eight_listeners_are_not_above_the_limit_for_a_weak_anchor(self):
        # Exactly 25 % is not more than 25 %, so these items still count.
        table = self.listeners()
        for name in ("L0", "L1"):
            table[name]["item0"]["anchor70"] = 95
            table[name]["item1"]["anchor70"] = 95
        result = analyze.post_screen(ratings_of(table))
        self.assertEqual(sorted(result.excluded), ["L0", "L1"])
        self.assertEqual(result.uninformative_anchor_items, [])


class Statistics(unittest.TestCase):
    def setUp(self):
        self.rng = np.random.default_rng(1)

    def test_interval_of_equal_values_has_no_width(self):
        self.assertEqual(analyze.bootstrap_interval(np.full(10, 70.0), self.rng), (70.0, 70.0))

    def test_interval_is_close_to_the_normal_one_for_many_values(self):
        values = np.random.default_rng(2).normal(50, 10, 400)
        low, high = analyze.bootstrap_interval(values, self.rng)
        half_width = 1.96 * np.std(values, ddof=1) / np.sqrt(len(values))
        # Narrow enough to tell 95 % from 90 %, whose half width would be 0.16 smaller here.
        self.assertAlmostEqual(low, np.mean(values) - half_width, delta=0.08)
        self.assertAlmostEqual(high, np.mean(values) + half_width, delta=0.08)

    def test_exact_test_counts_the_sign_assignments(self):
        # Only all plus and all minus reach the observed mean: 2 of 16.
        p = analyze.permutation_p_value(np.array([5.0, 5.0, 5.0, 5.0]), self.rng)
        self.assertAlmostEqual(p, 2 / 16)

    def test_exact_test_of_mixed_signs(self):
        # The means in reach of 2.0 or more, from all 16 sign assignments of 10, -2, 1, -1.
        differences = np.array([10.0, -2.0, 1.0, -1.0])
        expected = 0
        for mask in range(16):
            signs = np.array([1.0 if mask >> i & 1 else -1.0 for i in range(4)])
            expected += abs(np.mean(signs * differences)) >= 2.0
        p = analyze.permutation_p_value(differences, self.rng)
        self.assertAlmostEqual(p, expected / 16)

    def test_no_difference_gives_one(self):
        self.assertEqual(analyze.permutation_p_value(np.zeros(6), self.rng), 1.0)

    def test_many_listeners_use_random_assignments(self):
        differences = np.full(40, 3.0)
        self.assertLess(analyze.permutation_p_value(differences, self.rng), 0.01)

    def test_comparison_takes_the_mean_per_listener(self):
        table = {
            "a": {"x": {"model_64k": 90, "baseline_64k": 50}, "y": {"model_64k": 70, "baseline_64k": 50}},
            "b": {"x": {"model_64k": 60, "baseline_64k": 50}, "y": {"model_64k": 50, "baseline_64k": 50}},
        }
        c = analyze.compare(
            ratings_of(table), "model_64k", "baseline_64k", ["a", "b"], ["x", "y"], self.rng
        )
        # Listener a: 40 and 20, so 30. Listener b: 10 and 0, so 5.
        self.assertAlmostEqual(c.mean_difference, 17.5)
        self.assertEqual(c.listeners, 2)
        self.assertAlmostEqual(c.p_value, 2 / 4)
        self.assertEqual((c.low, c.high), (5.0, 30.0))

    def test_summary_of_known_values(self):
        s = analyze.summarize(np.array([10.0, 20.0, 30.0, 40.0, np.nan]), "x", self.rng)
        self.assertEqual((s.mean, s.median, s.count), (25.0, 25.0, 4))
        self.assertEqual((s.q1, s.q3), (17.5, 32.5))


class WholeAnalysis(unittest.TestCase):
    def test_writes_the_report_and_leaves_out_excluded_listeners(self):
        table = {f"L{i}": good_listener(ITEMS) for i in range(8)}
        for item in ITEMS:
            table["L0"][item] = {
                "reference": 10,
                "anchor70": 10,
                "anchor35": 10,
                "model_64k": 10,
                "baseline_64k": 90,
            }
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            screening, comparisons = analyze.analyze(ratings_of(table), out)
            report = (out / "results.md").read_text(encoding="utf-8")
            self.assertGreater((out / "scores.png").stat().st_size, 0)
        self.assertEqual(list(screening.excluded), ["L0"])
        self.assertEqual(len(comparisons), 1)
        # Without the excluded listener, everyone says the same.
        self.assertAlmostEqual(comparisons[0].mean_difference, 30.0)
        self.assertEqual(comparisons[0].listeners, 7)
        self.assertIn("Excluded by the post-screening: 1", report)
        self.assertIn("from 8 listeners", report)
        self.assertIn("| model_64k - baseline_64k | +30.0 |", report)
        self.assertIn("Item item7", report)
        # The excluded listener would pull this mean down from 80 and the one below up from 50.
        self.assertIn("| model_64k | 80.0 |", report)
        self.assertIn("| baseline_64k | 50.0 |", report)

    def test_an_item_only_an_excluded_listener_rated_does_not_appear(self):
        table = {f"L{i}": good_listener(ITEMS) for i in range(8)}
        for item in ITEMS:
            table["L0"][item]["reference"] = 10
        table["L0"]["extra"] = good_listener(["extra"])["extra"]
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            analyze.analyze(ratings_of(table), out)
            report = (out / "results.md").read_text(encoding="utf-8")
        self.assertNotIn("Item extra", report)

    def test_over_all_items_the_listeners_are_the_unit(self):
        # Eight listeners and eight items: eight values for the mean over all items, not 64.
        table = {f"L{i}": good_listener(ITEMS) for i in range(8)}
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            analyze.analyze(ratings_of(table), out)
            report = (out / "results.md").read_text(encoding="utf-8")
        overall = report[report.index("## All items") : report.index("## Item item0")]
        rows = [line for line in overall.splitlines() if line.startswith("| model_64k")]
        self.assertEqual(len(rows), 1)
        self.assertTrue(rows[0].endswith("| 8 |"), rows[0])


class Stimuli(unittest.TestCase):
    def setUp(self):
        self.rng = np.random.default_rng(3)

    def test_lowpass_keeps_what_is_below_the_cutoff(self):
        t = np.arange(48000) / metrics.SAMPLE_RATE
        tone = np.sin(2 * np.pi * 1000 * t)
        filtered = make_stimuli.lowpass(tone, 3500)
        middle = slice(4000, 44000)
        self.assertLess(np.max(np.abs(filtered[middle] - tone[middle])), 1e-3)

    def test_lowpass_removes_what_is_above_the_cutoff(self):
        t = np.arange(48000) / metrics.SAMPLE_RATE
        for cutoff, frequency in ((3500, 4000), (7000, 7500)):
            with self.subTest(cutoff=cutoff):
                filtered = make_stimuli.lowpass(np.sin(2 * np.pi * frequency * t), cutoff)
                self.assertLess(np.max(np.abs(filtered[4000:44000])), 1e-3)

    def test_lowpass_has_half_the_amplitude_at_the_cutoff(self):
        t = np.arange(48000) / metrics.SAMPLE_RATE
        filtered = make_stimuli.lowpass(np.sin(2 * np.pi * 3500 * t), 3500)
        self.assertAlmostEqual(float(np.max(np.abs(filtered[4000:44000]))), 0.5, delta=0.02)

    def test_lowpass_keeps_length_and_timing(self):
        click = np.zeros(10000)
        click[5000] = 1.0
        filtered = make_stimuli.lowpass(click, 3500)
        self.assertEqual(len(filtered), len(click))
        self.assertEqual(int(np.argmax(filtered)), 5000)

    def test_normalization_reaches_the_target_loudness(self):
        samples = 0.01 * self.rng.standard_normal(48000)
        result, gain = make_stimuli.normalized(samples, -33.0)
        self.assertAlmostEqual(gain, 10.0)
        self.assertAlmostEqual(result[1000] / samples[1000], 10 ** (10 / 20))

    def test_normalization_stays_below_the_peak_limit(self):
        samples = np.zeros(48000)
        samples[24000] = 0.5
        result, gain = make_stimuli.normalized(samples, -40.0)
        self.assertAlmostEqual(float(np.max(np.abs(result))), 10 ** (-1 / 20))
        self.assertLess(gain, 17.0)

    def test_normalization_fades_the_ends(self):
        result, _ = make_stimuli.normalized(np.full(48000, 0.1), -23.0)
        self.assertEqual(result[0], 0.0)
        self.assertLess(abs(result[-1]), 0.1 / 900)
        self.assertAlmostEqual(result[24000], 0.1)

    def write_items(self, folder: str, text: str) -> Path:
        path = Path(folder) / "items.csv"
        header = "name,file,start_seconds,duration_seconds,category,source,license\n"
        path.write_text(header + text, encoding="utf-8")
        return path

    def test_items_are_read(self):
        with tempfile.TemporaryDirectory() as folder:
            items = make_stimuli.read_items(
                self.write_items(folder, "a_1,a.wav,1.5,10,speech,https://example.org,CC0\n")
            )
        self.assertEqual(items[0].name, "a_1")
        self.assertEqual((items[0].start, items[0].duration), (1.5, 10.0))

    def test_invalid_items_are_rejected(self):
        cases = {
            "too long": "a,a.wav,0,13,speech,src,CC0\n",
            "no length": "a,a.wav,0,0,speech,src,CC0\n",
            "no source": "a,a.wav,0,10,speech,,CC0\n",
            "no license": "a,a.wav,0,10,speech,src, \n",
            "bad name": "a b,a.wav,0,10,speech,src,CC0\n",
            "twice": "a,a.wav,0,10,speech,src,CC0\na,b.wav,0,10,speech,src,CC0\n",
        }
        for name, text in cases.items():
            with self.subTest(name), tempfile.TemporaryDirectory() as folder:
                with self.assertRaises(ValueError):
                    make_stimuli.read_items(self.write_items(folder, text))

    def test_list_in_the_repository_is_valid(self):
        make_stimuli.read_items(Path(make_stimuli.__file__).with_name("items.csv"))

    def items(self) -> list[make_stimuli.Item]:
        return [
            make_stimuli.Item(name, f"{name}.wav", 0.0, 10.0, "music", "src", "CC0")
            for name in ("one", "two")
        ]

    def test_item_for_the_training_is_not_a_trial(self):
        items = [
            make_stimuli.Item("warmup", "w.wav", 0.0, 10.0, "training", "src", "CC0"),
            *self.items(),
        ]
        trials, training = make_stimuli.split_items(items)
        self.assertEqual(training, "warmup")
        self.assertEqual([item.name for item in trials], ["one", "two"])
        text = make_stimuli.configuration(trials, training)
        self.assertNotIn("id: trial_warmup", text)
        self.assertIn(f"reference: {make_stimuli.AUDIO_FOLDER}/warmup/reference.wav", text)

    def test_without_an_item_for_the_training_the_first_one_is_used(self):
        trials, training = make_stimuli.split_items(self.items())
        self.assertEqual(training, "one")
        self.assertEqual(len(trials), 2)

    def test_two_items_for_the_training_or_no_trials_are_refused(self):
        warmup = make_stimuli.Item("warmup", "w.wav", 0.0, 10.0, "training", "src", "CC0")
        with self.assertRaises(ValueError):
            make_stimuli.split_items([warmup, warmup, *self.items()])
        with self.assertRaises(ValueError):
            make_stimuli.split_items([warmup])

    def test_configuration_has_all_pages_in_order(self):
        text = make_stimuli.configuration(self.items(), "one")
        order = [
            "id: welcome",
            "id: volume",
            "id: training",
            "- random",
            "id: trial_one",
            "id: trial_two",
            "type: finish",
        ]
        positions = [text.index(entry) for entry in order]
        self.assertEqual(positions, sorted(positions))
        self.assertIn("testId: audio_codec_lab", text)
        self.assertIn("writeResults: true", text)

    def test_configuration_lists_every_condition_for_every_trial(self):
        text = make_stimuli.configuration(self.items(), "one")
        for item in ("one", "two"):
            folder = f"{make_stimuli.AUDIO_FOLDER}/{item}"
            self.assertIn(f"reference: {folder}/reference.wav", text)
            for condition in make_stimuli.CONDITIONS:
                self.assertIn(f"{condition.name}: {folder}/{condition.name}.wav", text)

    def test_configuration_keeps_the_test_blind_and_randomized(self):
        text = make_stimuli.configuration(self.items(), "one")
        self.assertEqual(text.count("showConditionNames: false"), 3)
        self.assertEqual(text.count("randomize: true"), 3)
        self.assertNotIn("showConditionNames: true", text)
        self.assertNotIn("randomize: false", text)
        self.assertNotIn("showResults: true", text)

    def test_configuration_uses_its_own_anchors(self):
        text = make_stimuli.configuration(self.items(), "one")
        self.assertEqual(text.count("createAnchor35: false"), 3)
        self.assertEqual(text.count("createAnchor70: false"), 3)

    def test_the_participants_are_told_that_all_sliders_start_at_100(self):
        # webMUSHRA enforces nothing here, and a slider left alone counts as no difference.
        self.assertIn("anfangs auf 100", make_stimuli.TEXT["training"])
        self.assertIn("jeden Regler bewusst", make_stimuli.TEXT["trial"])

    def write_service(self, folder: str, text: bytes) -> Path:
        path = Path(folder) / "service" / "write.php"
        path.parent.mkdir()
        path.write_bytes(text)
        return path

    def test_the_result_service_is_repaired_for_php_8(self):
        # The form in release 1.4.3, with a byte that is not valid UTF-8 in a comment.
        original = (
            b"<?php\r\n// (C) \xe9\r\n$sessionParam = null;\r\n"
            b"if(get_magic_quotes_gpc()){\r\n"
            b"  $sessionParam = stripslashes($_POST['sessionJSON']);\r\n"
            b"}else{\r\n"
            b"  $sessionParam = $_POST['sessionJSON'];\r\n"
            b"}\r\n"
            b"$session = json_decode($sessionParam);\r\n"
            b"$length = count($session->participant->name);\r\n"
            b"for($i = 0; $i < count($trial->stimulusRating); $i++){\r\n"
        )
        with tempfile.TemporaryDirectory() as folder:
            path = self.write_service(folder, original)
            make_stimuli.repair_result_service(Path(folder))
            repaired = path.read_bytes()
            # Repairing twice changes nothing.
            make_stimuli.repair_result_service(Path(folder))
            self.assertEqual(path.read_bytes(), repaired)
        self.assertNotIn(b"get_magic_quotes_gpc", repaired)
        self.assertNotIn(b"stripslashes", repaired)
        self.assertIn(b"$sessionParam = $_POST['sessionJSON'];", repaired)
        self.assertIn(b"\xe9", repaired)
        self.assertIn(b"$session = json_decode($sessionParam);\r\n", repaired)
        self.assertIn(b"$length = count((array)$session->participant->name);", repaired)
        self.assertIn(b"$i < count((array)$trial->stimulusRating);", repaired)

    def test_a_result_service_of_an_unknown_form_is_reported(self):
        with tempfile.TemporaryDirectory() as folder:
            self.write_service(folder, b"<?php\n$x = get_magic_quotes_gpc();\n")
            with self.assertRaises(ValueError):
                make_stimuli.repair_result_service(Path(folder))

    def test_a_folder_without_a_result_service_is_fine(self):
        with tempfile.TemporaryDirectory() as folder:
            make_stimuli.repair_result_service(Path(folder))

    def test_too_many_conditions_are_refused(self):
        many = tuple(make_stimuli.Condition(f"c{i}", "model", 64000) for i in range(12))
        with self.assertRaises(ValueError):
            make_stimuli.configuration(self.items(), "one", many)

    def test_conditions_fit_the_analysis(self):
        names = {condition.name for condition in make_stimuli.CONDITIONS}
        self.assertIn(analyze.MID_ANCHOR, names)
        self.assertIn(analyze.LOW_ANCHOR, names)
        self.assertNotIn(analyze.HIDDEN_REFERENCE, names)
        for first, second in analyze.COMPARED_PAIRS:
            self.assertIn(first, names)
            self.assertIn(second, names)


class _RangeServer(http.server.BaseHTTPRequestHandler):
    """Serves one file. With the flag on the server, it ignores range requests as some servers do."""

    def do_GET(self):
        data = self.server.data
        requested = self.headers.get("Range")
        if requested and self.server.supports_range:
            first, last = requested.removeprefix("bytes=").split("-")
            part = data[int(first) : int(last) + 1]
            self.send_response(206)
            self.send_header("Content-Range", f"bytes {first}-{last}/{len(data)}")
        else:
            part = data
            self.send_response(200)
        self.send_header("Content-Length", str(len(part)))
        self.end_headers()
        self.wfile.write(part)

    def log_message(self, format, *args):
        pass


class FetchItems(unittest.TestCase):
    def serve(self, data: bytes, supports_range: bool = True) -> str:
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), _RangeServer)
        server.data = data
        server.supports_range = supports_range
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(thread.join)
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        return f"http://127.0.0.1:{server.server_address[1]}/ODAQ.zip"

    @staticmethod
    def archive() -> bytes:
        stream = io.BytesIO()
        with zipfile.ZipFile(stream, "w", zipfile.ZIP_DEFLATED) as file:
            file.writestr(fetch_items.FOLDER_IN_ARCHIVE + "a.wav", b"first" * 1000)
            file.writestr(fetch_items.FOLDER_IN_ARCHIVE + "b.wav", b"second" * 1000)
            file.writestr(fetch_items.FOLDER_IN_ARCHIVE + "not_wanted.wav", b"third")
            for name in fetch_items.LICENSE_FILES:
                file.writestr(name, name.encode())
        return stream.getvalue()

    def test_only_the_wanted_files_and_the_license_notes_arrive(self):
        with tempfile.TemporaryDirectory() as folder:
            target = Path(folder)
            written = fetch_items.fetch(self.serve(self.archive()), ["a.wav", "b.wav"], target)
            self.assertEqual((target / "a.wav").read_bytes(), b"first" * 1000)
            self.assertEqual((target / "b.wav").read_bytes(), b"second" * 1000)
            self.assertFalse((target / "not_wanted.wav").exists())
            self.assertIn("_license_disclaimer.txt", written)
            self.assertTrue((target / "_detailed_license.csv").exists())

    def test_files_that_are_already_there_are_kept(self):
        with tempfile.TemporaryDirectory() as folder:
            target = Path(folder)
            url = self.serve(self.archive())
            fetch_items.fetch(url, ["a.wav"], target)
            self.assertEqual(fetch_items.fetch(url, ["a.wav"], target), [])

    def test_a_damaged_local_file_is_fetched_again(self):
        with tempfile.TemporaryDirectory() as folder:
            target = Path(folder)
            url = self.serve(self.archive())
            fetch_items.fetch(url, ["a.wav"], target)
            (target / "a.wav").write_bytes(b"short")
            self.assertEqual(fetch_items.fetch(url, ["a.wav"], target), ["a.wav"])
            self.assertEqual((target / "a.wav").read_bytes(), b"first" * 1000)

    def test_a_missing_file_is_reported(self):
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaises(KeyError):
                fetch_items.fetch(self.serve(self.archive()), ["missing.wav"], Path(folder))

    def test_a_server_without_range_requests_is_reported(self):
        with tempfile.TemporaryDirectory() as folder:
            url = self.serve(self.archive(), supports_range=False)
            with self.assertRaises(OSError):
                fetch_items.fetch(url, ["a.wav"], Path(folder))

    def test_the_list_in_the_repository_names_the_files(self):
        files = fetch_items.wanted_files(Path(fetch_items.__file__).with_name("items.csv"))
        self.assertEqual(len(files), 9)
        self.assertIn("27_castanets.wav", files)


class WithRealTools(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        folder = os.environ.get("CODEC_TOOLS_DIR")
        if not folder:
            raise RuntimeError("Name the folder of the tools with the variable CODEC_TOOLS_DIR.")
        if not shutil.which("ffmpeg"):
            raise RuntimeError("These tests need ffmpeg.")
        cls.temporary = tempfile.TemporaryDirectory()
        root = Path(cls.temporary.name)
        cls.out = root / "stimuli"
        source = root / "source"
        source.mkdir()
        # A second longer than the excerpt, in stereo at 44.1 kHz, to check the conversion.
        rate = 44100
        t = np.arange(4 * rate) / rate
        left = 0.1 * np.sin(2 * np.pi * 440 * t)
        stereo = np.stack([left, left], axis=1)
        with wave.open(str(source / "tone.wav"), "wb") as file:
            file.setnchannels(2)
            file.setsampwidth(2)
            file.setframerate(rate)
            file.writeframes(np.round(stereo * 32767).astype("<i2").tobytes())
        signals.write_item(source / "clicks.wav", "clicks", 4.0)
        cls.source = source
        cls.items = [
            make_stimuli.Item("tone", "tone.wav", 1.0, 2.0, "tonal", "made here", "none needed"),
            make_stimuli.Item("clicks", "clicks.wav", 0.5, 3.0, "transient", "made here", "none needed"),
        ]
        make_stimuli.make_stimuli(cls.items, Path(folder).resolve(), source, cls.out)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def names(self) -> list[str]:
        return ["reference"] + [c.name for c in make_stimuli.CONDITIONS]

    def test_all_files_have_the_format_and_length_of_the_excerpt(self):
        for item in self.items:
            for name in self.names():
                with self.subTest(item=item.name, condition=name):
                    with wave.open(str(self.out / item.name / f"{name}.wav"), "rb") as file:
                        self.assertEqual(file.getframerate(), metrics.SAMPLE_RATE)
                        self.assertEqual(file.getnchannels(), 1)
                        self.assertEqual(file.getsampwidth(), 2)
                        self.assertEqual(file.getnframes(), round(item.duration * metrics.SAMPLE_RATE))

    def test_excerpt_starts_where_the_list_says(self):
        original = metrics.read_wav(self.source / "clicks.wav")
        reference = metrics.read_wav(self.out / "clicks" / "reference.wav")
        start = round(0.5 * metrics.SAMPLE_RATE)
        expected = original[start : start + len(reference)]
        middle = slice(4800, len(reference) - 4800)
        correlation = np.corrcoef(expected[middle], reference[middle])[0, 1]
        self.assertGreater(correlation, 0.999)

    def test_no_stimulus_clips(self):
        for item in self.items:
            for name in self.names():
                with self.subTest(item=item.name, condition=name):
                    samples = metrics.read_wav(self.out / item.name / f"{name}.wav")
                    self.assertLess(float(np.max(np.abs(samples))), 0.999)

    def test_reference_reaches_the_target_loudness(self):
        loudness = make_stimuli.loudness_lufs(self.out / "tone" / "reference.wav")
        self.assertAlmostEqual(loudness, make_stimuli.TARGET_LUFS, delta=1.0)

    def test_all_conditions_are_in_time_with_the_reference(self):
        reference = metrics.read_wav(self.out / "clicks" / "reference.wav")
        for name in self.names()[1:]:
            with self.subTest(name):
                decoded = metrics.read_wav(self.out / "clicks" / f"{name}.wav")
                size = len(reference) + len(decoded)
                spectrum = np.fft.rfft(decoded, size) * np.conj(np.fft.rfft(reference, size))
                self.assertEqual(int(np.argmax(np.fft.irfft(spectrum, size))), 0)

    def test_conditions_are_what_their_names_say(self):
        folder = self.out / "clicks"
        reference = metrics.read_wav(folder / "reference.wav")

        def energy_above(name: str, hertz: float) -> float:
            samples = metrics.read_wav(folder / f"{name}.wav")
            spectrum = np.abs(np.fft.rfft(samples)) ** 2
            frequencies = np.fft.rfftfreq(len(samples), 1 / metrics.SAMPLE_RATE)
            return float(np.sum(spectrum[frequencies > hertz]) / np.sum(spectrum))

        self.assertLess(energy_above("anchor35", 4000), 1e-5)
        self.assertLess(energy_above("anchor70", 7500), 1e-5)
        self.assertGreater(energy_above("reference", 7500), 0.1)

        def snr(name: str) -> float:
            return metrics.snr_db(reference, metrics.read_wav(folder / f"{name}.wav"))

        # A higher bitrate is closer to the original, and flat noise has the higher SNR.
        self.assertGreater(snr("model_64k"), snr("model_32k"))
        self.assertGreater(snr("baseline_64k"), snr("baseline_32k"))
        self.assertGreater(snr("baseline_64k"), snr("model_64k"))

    def test_configuration_is_written_with_the_items(self):
        text = (self.out / "mushra.yaml").read_text(encoding="utf-8")
        self.assertIn("id: trial_tone", text)
        self.assertIn("id: trial_clicks", text)
        self.assertIn(f"stimulus: {make_stimuli.AUDIO_FOLDER}/tone/reference.wav", text)

    def test_originals_are_collected_for_the_evaluation(self):
        folder = self.out / make_stimuli.REFERENCES_FOLDER
        self.assertEqual(sorted(p.name for p in folder.iterdir()), ["clicks.wav", "tone.wav"])
        self.assertEqual(
            (folder / "clicks.wav").read_bytes(),
            (self.out / "clicks" / "reference.wav").read_bytes(),
        )

    def test_nothing_is_left_of_the_work_folder(self):
        self.assertFalse((self.out / "_work").exists())

    def test_installing_copies_all_that_webmushra_reads(self):
        with tempfile.TemporaryDirectory() as folder:
            webmushra = Path(folder)
            (webmushra / "configs").mkdir()
            (webmushra / "service").mkdir()
            service = webmushra / "service" / "write.php"
            service.write_text(
                "if(get_magic_quotes_gpc()){\n  $a = stripslashes($b);\n}else{\n  $a = $b;\n}\n",
                encoding="ascii",
            )
            make_stimuli.install_into_webmushra(self.out, webmushra, self.items)
            self.assertNotIn("get_magic_quotes_gpc", service.read_text(encoding="ascii"))
            self.assertTrue((webmushra / "configs" / f"{make_stimuli.TEST_ID}.yaml").exists())
            audio = webmushra / make_stimuli.AUDIO_FOLDER
            for item in self.items:
                for name in self.names():
                    self.assertTrue((audio / item.name / f"{name}.wav").exists(), name)


if __name__ == "__main__":
    unittest.main()
