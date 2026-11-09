# Evaluation

Runs the codec with and without its model, and AAC and Opus through ffmpeg, on a set of signals and
measures the results. The numbers and figures in [results](results) come from these scripts.

**What this does not show:** it says nothing certain about how the codec sounds. All measures here are
computed, and nobody has taken the listening test yet, which is the only way to answer whether the model
is an improvement. By the one independent measure so far, ViSQOL, the model is ahead of flat noise on most
real excerpts from 32 to 64 kbit/s and behind it at 16 and 96 kbit/s, see [real music](#real-music).

## Run it

You need the built tools, Python 3.12 or newer, and optionally ffmpeg with libopus. Without ffmpeg,
the comparison with AAC and Opus is left out.

```bash
python -m pip install -r eval/requirements.txt
python eval/evaluate.py --tools build/default --out eval/out
```

This takes about a minute. On Windows, the tools are in `build/default/Release`.

| Option | Meaning |
|---|---|
| `--items folder` | Use WAV files (16 bit, 48 kHz, mono) instead of the synthetic signals |
| `--bitrates 32000 64000` | Other target bitrates, in bit/s |
| `--duration 4` | Length of the synthetic signals in seconds |
| `--visqol folder` | Also rate all files with ViSQOL, from the folder that `build_visqol.sh` created |

[ViSQOL](https://github.com/google/visqol) has no ready-made binaries.
[build_visqol.sh](build_visqol.sh) builds it on Linux with the flags that GCC 15 needs and checks that it
reaches the expected score of ViSQOL's own conformance test, 2.34687. This takes up about 3 GB and about
15 minutes, so CI does not do it, and its tests of this path use a stand-in for the program. The results
in this repository were generated with the real one.

To test the scripts, name the folder of the tools:

```bash
CODEC_TOOLS_DIR=build/default python -m unittest discover --start-directory eval
```

## Signals

[signals.py](signals.py) generates the synthetic ones, so that no licensed audio is needed. Each one
stresses one thing: a single tone, harmonics with quiet gaps, decaying chords, pink noise, white noise,
and short bursts that provoke pre-echo. They are stand-ins and do not replace real music.

The real music is the eight excerpts of the [listening test](../listening-test/README.md#audio). These
commands fetch them and measure them, with ffmpeg and the folder of ViSQOL as above:

```bash
python listening-test/fetch_items.py
python listening-test/make_stimuli.py --tools build/default
python eval/evaluate.py --tools build/default --items listening-test/stimuli/references --out eval/results/music --visqol ~/visqol-build/visqol
```

## Measures

| Measure | Meaning |
|---|---|
| SNR | Energy of the signal over energy of the error, for the whole file |
| Segmental SNR | Mean SNR of 20 ms segments, limited to -10 to 35 dB, without silent segments |
| Mean excess | Mean over all frames and bands of how many dB the noise lies above the masking threshold. Bands below it count as 0. |
| Bands above | Share of all bands in all frames with noise above the masking threshold |
| ViSQOL | The MOS-LQO of ViSQOL in its audio mode, a prediction of the score in a listening test, up to about 4.75 |
| Bitrate | Size of the file over its duration, including the container for AAC and Opus |

The noise always comes from the decoded file: the difference to the original goes through the same
framing and MDCT as in the codec, and the energies are summed per band. So every codec is measured
the same way. Tests check that this MDCT equals the one in the codec and that for this codec the
result agrees per frame and band with what the encoder reports, apart from the rounding to 16 bits.

The tables also include AAC and Opus, but **the excess cannot rank this codec against them**. It uses
the thresholds of this codec's own model, which this encoder aims at and the others do not. It also
compares waveforms, and AAC and Opus may replace noisy parts with synthetic noise, which counts as an
error here but may sound fine. AAC is the encoder built into ffmpeg, not the best one available. Its
bitrate overshoots at low targets, up to 28 instead of 16 kbit/s, so compare with the real bitrates in
the last table. Opus runs in hard CBR, since its usual VBR mode took up to twice the target on tonal
signals.

ViSQOL does not depend on this codec's model, so it is the one measure here that can compare all
codecs. It is still only a prediction, trained on real music, so the synthetic signals are unlike what
it was trained on.

## Results

All tables for the synthetic signals are in [results/results.md](results/results.md), and those for real
music in [results/music/results.md](results/music/results.md). They were generated with ffmpeg 8.0.1. The
numbers for this codec were identical on another machine in CI. Those for AAC and Opus differed there by
up to 2.4 dB of SNR, probably because of another version of ffmpeg, so small differences between them
mean little.

### Real music

![ViSQOL scores of all codecs for eight excerpts](results/music/visqol.png)

These are means over the eight excerpts. The columns "old limit" show the codec before the change that
[the next section](#how-the-bandwidth-limit-was-chosen) describes, at commit `b0b4262`.

| kbit/s | ViSQOL: model | old limit | Flat noise | AAC | Opus | Excess in dB: model | old limit | Flat noise |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 16 | 1.80 | 1.78 | 2.61 | 1.79 | 3.95 | 4.09 | 3.48 | 3.20 |
| 32 | 3.96 | 2.04 | 3.26 | 3.40 | 4.19 | 2.36 | 1.44 | 1.62 |
| 48 | 4.14 | 3.25 | 3.74 | 4.28 | 4.43 | 0.74 | 0.55 | 0.86 |
| 64 | 4.22 | 4.22 | 4.08 | 4.42 | 4.52 | 0.22 | 0.22 | 0.49 |
| 96 | 4.27 | 4.27 | 4.43 | 4.60 | 4.67 | 0.01 | 0.01 | 0.15 |

- **Overall, it is a draw.** By ViSQOL, the model and flat noise are each ahead in 20 of the 40 cases, but
  not evenly: the model on 0 of the 8 excerpts at 16 kbit/s, on 7 at 32, on 7 at 48, on 5 at 64 and on 1
  at 96 kbit/s.
- **At 64 kbit/s, the model is ahead by the mean values of all three measures:** ViSQOL, the mean excess,
  and the bands above the threshold (8 % against 12 %).
- **At 48 kbit/s, it is ahead by ViSQOL and the mean excess**, but more bands lie above the threshold, 31 %
  against 20 %, by less on average.
- **At 32 kbit/s, the measures disagree.** ViSQOL prefers the model (3.96 against 3.26), but its mean
  excess is higher (2.36 against 1.62 dB), with 64 % of the bands above the threshold against 33 %.
- **At 16 kbit/s, the model is behind** by ViSQOL, the mean excess and the bands above the threshold.
- **At 96 kbit/s, flat noise is ahead by ViSQOL** (4.43 against 4.27). Keeping the bands below the threshold
  in quiet gave 4.35 in a modified copy of the encoder, so that is a part of the reason, but not all of it.
- **The glockenspiel stays with flat noise** at every bitrate, for example 4.30 against 4.53 at 32 kbit/s.
- **Opus is ahead of this codec at every bitrate**, and AAC from 48 kbit/s on, which is no surprise for a
  small codec without block switching, noise filling or a bit reservoir.

![Signal, threshold and noise per band for jazz at 48 kbit/s](results/music/masking_jazz_48k.png)

This is the plot of where the noise goes, and it shows the price of the wider bandwidth. With the model, the
noise follows the masking threshold closely up to the bandwidth limit, here at 15.4 kHz, but on this dense
excerpt it lies right on the threshold rather than below it. Flat noise leaves the low bands with a margin
that is not needed and crosses the threshold from about 7 kHz on. The figures for all excerpts are in
[results/music](results/music), and those for the synthetic signals in [results](results).

### How the bandwidth limit was chosen

The limit at first allowed 1.75 bits per coded coefficient, chosen by the mean excess on synthetic signals.
Then ViSQOL preferred flat noise in 32 of the 40 cases on real excerpts. Modified copies of the encoder
showed that the limit was the main reason. With the same limit, flat noise fell to the scores of the model:

| Synthetic signal and bitrate | Old limit | No limit | Flat noise | Flat noise with old limit |
|---|---:|---:|---:|---:|
| White noise, 32 kbit/s | 1.00 | 3.47 | 3.66 | 1.00 |
| Pink noise, 48 kbit/s | 2.05 | 3.59 | 3.43 | 2.04 |
| Harmonic, 16 kbit/s | 2.37 | 3.75 | 4.48 | 2.36 |

So several values were compared on the real excerpts. Each cell holds the mean ViSQOL score and then the mean
excess in dB:

| Bits per coefficient | 1.75 | 1.25 | 1.0 | 0.75 | 0.5 | No limit | Flat noise |
|---|---:|---:|---:|---:|---:|---:|---:|
| 16 kbit/s | 1.78, 3.48 | 1.76, 3.89 | 1.80, 4.09 | 2.35, 4.41 | 2.85, 4.54 | 3.36, 4.70 | 2.61, 3.20 |
| 32 kbit/s | 2.04, 1.44 | 3.17, 1.95 | 3.96, 2.36 | 3.96, 2.36 | 3.96, 2.36 | 3.96, 2.36 | 3.26, 1.62 |
| 48 kbit/s | 3.25, 0.55 | 4.14, 0.74 | 4.14, 0.74 | 4.14, 0.74 | 4.14, 0.74 | 4.14, 0.74 | 3.74, 0.86 |

At 1 bit, the whole gain at 32 and 48 kbit/s is there, and nothing looser adds to it, apparently because
the excerpts lie below the threshold in quiet above 15.4 kHz. The price is the higher excess at 32 kbit/s.
That is a choice between dull and noisier, which measures cannot settle. One person compared the versions by
ear at 32 kbit/s, on a page with the versions in random order, and preferred 1 bit. That is no test result,
so the listening test includes the model with the earlier limit at 32 kbit/s as a condition of its own. The
encoder option `--bandwidth-limit 1.75` produces it. The comparisons in this section do not come from the
scripts here.

### Synthetic signals

![ViSQOL scores of all codecs for six signals](results/visqol.png)

ViSQOL prefers flat noise to the model in 23 of the 30 cases here, the model in 6. The broadband signals
still have energy above the limit, so they lose more than real excerpts do: on white noise at 48 kbit/s it
gives 2.12 against 4.02. On the noise signals, flat noise also beats AAC and Opus, which replace noise with
something that only sounds similar. That hints that ViSQOL rewards the exact waveform on such signals, so
these scores should not be taken too literally.

The clicks have a cause of their own. In the decoded file, the part between the clicks was replaced with the
original, then the part before each attack, then the clicks. At 96 kbit/s, the first lifts the model from
3.21 to 4.49, level with flat noise at 4.51, while the second adds 0.12 and the third nothing. So on this
signal ViSQOL does not object to pre-echo, as might be expected, but to the background at about -72 dBFS,
which the model holds to be inaudible and does not code. That may well be right at a normal listening level.
This was one separate check; the scripts do not produce these figures.

![SNR, segmental SNR and mean excess for six signals](results/measures.png)

The computed measures disagree as well, which is the point of showing them together. At 48 kbit/s on the
chords, the SNR drops from 50.7 to 14.2 dB with the model. On white noise, the mean excess drops from 3.83 to
1.28 dB. The excess favors the model by construction, since the encoder aims at exactly these thresholds. So
this is evidence that the model does what it is designed to do, not that it sounds better.

### Known weaknesses

- **Pre-echo.** A frame spreads its noise over its whole window. Before an attack that is clearly
  visible, and it may well be audible, whatever ViSQOL says. There is no block switching.

  ![Decoded signal around a burst, with noise before the attack](results/pre_echo_clicks_48k.png)

- **Attacks get too few bits.** On the synthetic chords at 48 kbit/s, the frames within one frame of a note
  start are 9 % of all frames. They hold 97 % of the noise energy, and 36 % of their bands lie above the
  threshold, against 1 % elsewhere. In the other frames, the noise lies 29 dB below the threshold in the
  median. So bits are left over there while the attacks run short, because each frame has the same fixed
  budget. A bit reservoir could address this. These figures come from one separate check of the
  `codec-analyze` table and the decoded file; the scripts do not produce them.
- **Broadband noise at low bitrates.** Few bits spread over a wide band leave much of it above the threshold:
  on synthetic pink noise at 32 kbit/s, the mean excess is 3.59 dB against 1.93 dB with flat noise. This is
  the price of the wider bandwidth.
- **Very low bitrates.** At 16 kbit/s, ViSQOL gives the model only 1.5 to 2.3 on the real excerpts, and flat
  noise is ahead on all of them.

## The table of codec-analyze

`codec-analyze in.wav out.csv --bitrate 64000 [--psy on|off]` runs the encoder and writes one line per
frame and band. All energies are on the scale of the MDCT coefficients, where a full scale sine has an
energy of 512 per frame, which the model equates with 96 dB SPL.

| Column | Meaning |
|---|---|
| `frame`, `band` | Position in the signal and in the spectrum |
| `first_coefficient`, `last_coefficient` | The coefficients of the band |
| `energy` | Energy of the signal in the band |
| `masking_threshold`, `quiet_threshold` | Thresholds of the model, also listed with `--psy off` |
| `noise` | Energy of the quantization error in the band |
| `payload_bytes`, `budget_bits` | Size of the frame and the bits it was allowed to use |
