# Listening test

A MUSHRA test following [ITU-R BS.1534-3](https://www.itu.int/rec/R-REC-BS.1534). It answers the main
question of this project: does the codec with its masking model sound better than the same codec with
flat noise at the same bitrate?

**Status: prepared, not yet carried out.** The scripts are tested, but only with made-up ratings, and
no real listener has taken this test. The audio has not been fetched yet, and the webMUSHRA setup has
not been run end to end. There are no results to report.

## Design

Each trial compares up to eight signals with the original. webMUSHRA shows them under neutral names in
random order, and the trials are in random order as well.

| Condition | Meaning |
|---|---|
| `reference` | The original, hidden among the others. webMUSHRA adds it by itself. |
| `anchor35`, `anchor70` | Low-pass filtered at 3.5 and 7 kHz, the two anchors of BS.1534 |
| `model_32k`, `model_64k` | This codec with the masking model |
| `baseline_32k`, `baseline_64k` | This codec with flat noise, at the same bitrates |
| `opus_64k` | Opus in hard CBR, as a known point of comparison |

The plan is 8 to 12 listeners with good headphones, and about 30 to 40 minutes for each, including a
training trial that does not count. The excerpts last 8 to 10 s, as BS.1534 recommends.

**Known limits:** the codec works in mono, so everything is mixed down to mono and the test says nothing
about stereo. The excerpts are brought to the same loudness by measurement (EBU R 128, -23 LUFS), where
BS.1534 asks for a group of experts to agree on it. Most listeners will not be trained experts, which
makes small differences harder to detect.

## Audio

The excerpts come from [ODAQ](https://github.com/Fraunhofer-IIS/ODAQ), the Open Dataset of Audio Quality
by Fraunhofer IIS and Netflix, which has this kind of critical material under Creative Commons licenses.
The audio stays out of this repository; `fetch_items.py` reads just these files from the 1 GB archive
on [Zenodo](https://doi.org/10.5281/zenodo.10405774), about 18 MB in total.

| Item | Content | Source | License |
|---|---|---|---|
| claves | Wooden claves, for the training only | [Samuel Gremaud](https://freesound.org/people/SamuelGremaud/sounds/517609/) | CC0 1.0 |
| castanets | Castanets, which provoke pre-echo | [Satoration](https://freesound.org/people/Satoration/sounds/57299/) | CC BY 4.0 |
| glockenspiel | Tones with sharp attacks | [Anasas6](https://freesound.org/people/Anasas6/sounds/502882/) | CC0 1.0 |
| violin | Sustained tones | [FreqMan](https://freesound.org/people/FreqMan/sounds/42955/) | CC BY 4.0 |
| accordion | Many tones at once | [eyenorth](https://freesound.org/people/eyenorth/sounds/482213/) | CC BY 4.0 |
| jazz | Dense music | [Scott D. Hampton](https://soundcloud.com/scottdhampton/byrd-in-hand-extended) | CC BY 3.0 |
| choir | Singing | [klankbeeld](https://freesound.org/people/klankbeeld/sounds/263612/) | CC BY 4.0 |
| speech_female | Female speech | Fraunhofer IIS, part of ODAQ | CC BY 4.0 |
| speech_male | Male speech | [Nina Paley, Sita Sings the Blues](https://archive.org/details/Sita_Sings_the_Blues) | CC0 1.0 |

ODAQ cut and scaled these files. For the test they are also resampled to 48 kHz, mixed down to mono,
scaled again, faded at the ends, and coded by the codecs above. If you use ODAQ, cite its authors:
M. Torcoli, C.-W. Wu, S. Dick, P. A. Williams, M. M. Halimeh, W. Wolcott and E. A. P. Habets,
"ODAQ: Open Dataset of Audio Quality," ICASSP 2024.

## Prepare and run the test

You need the built tools, ffmpeg with libopus, and the packages of `eval/requirements.txt`. These
commands fetch the audio and create the stimuli in `stimuli/`:

```bash
python listening-test/fetch_items.py
python listening-test/make_stimuli.py --tools build/default
```

The test itself runs on [webMUSHRA](https://github.com/audiolabs/webMUSHRA), which is not part of this
repository. Unpack release 1.4.3 into `listening-test/webmushra`, which Git ignores, and copy everything
into it:

```bash
python listening-test/make_stimuli.py --tools build/default --webmushra listening-test/webmushra
```

Then start its server, which needs PHP, and open the address in Chrome:

```bash
php -S localhost:8000 -t listening-test/webmushra
```

```text
http://localhost:8000/?config=audio_codec_lab.yaml
```

webMUSHRA stops with an error if the sound device does not run at the rate of the files. In the settings
of the device, set it to 48 kHz. On the same computer, each listener needs a different short name when
asked for one, because release 1.4.3 tells them apart by it.

## Evaluate the results

webMUSHRA writes `results/audio_codec_lab/mushra.csv` in its own folder. Then:

```bash
python listening-test/analyze.py listening-test/webmushra/results/audio_codec_lab/mushra.csv
```

This writes `results/results.md` and `results/scores.png` and follows section 4.1.2 and chapter 9 of
BS.1534-3:

- **Post-screening.** A listener is excluded for rating the hidden reference below 90, or the 7 kHz
  anchor above 90, on more than 15 % of the items. If over 25 % of the listeners rate the anchor above 90
  on an item, then that item counts against nobody.
- **Means and medians.** The 95 % intervals come from bootstrapping, because a small sample may not be
  normally distributed. Over all items, the mean per listener is the unit, since several ratings by one
  person are not independent.
- **Model against baseline.** The difference per listener at the same bitrate, with a test of
  randomization: exact for up to 16 listeners, from 10000 random assignments above that.

To test the scripts:

```bash
CODEC_TOOLS_DIR=build/default python -m unittest discover --start-directory listening-test
```
