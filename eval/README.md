# Evaluation

Runs the codec with and without its model, and AAC and Opus through ffmpeg, on a set of signals and
measures the results. The numbers and figures in [results](results) come from these scripts.

**What this does not show:** it says nothing certain about how the codec sounds. All measures here are
computed, on synthetic signals. Real music and a listening test are still missing, and only the
listening test can answer whether the model is an improvement. So far, the one independent measure
speaks against it on most of these signals, see [below](#visqol-disagrees).

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

[signals.py](signals.py) generates all of them, so that no licensed audio is needed. Each one stresses
one thing: a single tone, harmonics with quiet gaps, decaying chords, pink noise, white noise, and
short bursts that provoke pre-echo. They are stand-ins and do not replace real music.

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
codecs. It is still only a prediction, trained on real music, and these synthetic signals are unlike
what it was trained on.

## Results

All tables are in [results/results.md](results/results.md). They were generated with ffmpeg 8.0.1. The
numbers for this codec were identical on another machine in CI. Those for AAC and Opus differed there by
up to 2.4 dB of SNR, probably because of another version of ffmpeg, so small differences between them
mean little.

![Signal, threshold and noise per band for pink noise at 48 kbit/s](results/masking_pink_noise_48k.png)

With the model, the noise follows the masking threshold and stays below it up to the bandwidth limit,
which is 11.4 kHz at 48 kbit/s. The two top bands are not coded, so their whole energy counts as noise.
Flat noise leaves the low bands with a margin that is not needed and crosses the threshold from about
5 kHz on. The figures for the other signals are in the same folder.

![SNR, segmental SNR and mean excess for six signals](results/measures.png)

The measures disagree, which is the point of showing all of them. At 48 kbit/s on the chords, the SNR
drops from 50.7 to 15.1 dB with the model. On white noise, the bands above the threshold drop from
95.9 % to 11.8 %. The excess favors the model by construction, since the encoder aims at exactly these
thresholds. So this is evidence that the model does what it is designed to do, not that it sounds
better.

### ViSQOL disagrees

![ViSQOL scores of all codecs for six signals](results/visqol.png)

ViSQOL prefers flat noise to the model in 25 of the 30 cases, in places by a wide margin: on white noise
at 32 kbit/s it gives 1.00 against 3.66, on pink noise at 48 kbit/s 2.05 against 3.43. The model is ahead
only on the chords from 48 kbit/s on and on pink noise at 96 kbit/s. On the noise signals, flat noise also
beats AAC and Opus, which replace noise with something that only sounds similar. That hints that ViSQOL
rewards the exact waveform on such signals, so these scores should not be taken too literally.

Counter-checks with modified copies of the encoder show two causes. The scripts do not produce these
figures, and these changes are not in the codec.

| Signal and bitrate | Model | Model without limit | Flat noise | Flat noise with limit |
|---|---:|---:|---:|---:|
| White noise, 32 kbit/s | 1.00 | 3.47 | 3.66 | 1.00 |
| Pink noise, 48 kbit/s | 2.05 | 3.59 | 3.43 | 2.04 |
| Harmonic, 16 kbit/s | 2.37 | 3.75 | 4.48 | 2.36 |
| Clicks, 96 kbit/s, where no limit applies | 3.21 | 3.21 | 4.26 | 4.26 |

- **The bandwidth limit** is the main cause. With the same limit, flat noise falls to the scores of the
  model, and without it the model moves ahead on pink noise. But the two measures pull in
  opposite directions: without the limit, the mean excess on pink noise at 32 kbit/s rises from 1.02 to
  4.57 dB. So it is open which is right, the clean but dull sound with the limit or the full bandwidth
  with more audible noise. That takes listening.
- **The quiet background** explains the clicks. In the decoded file, the part between the clicks was
  replaced with the original, then the part before each attack, then the clicks. At 96 kbit/s, the first
  lifts the model from 3.21 to 4.49, level with flat noise at 4.51, while the second adds 0.12 and the
  third nothing. So on this signal ViSQOL does not object to pre-echo, as might be expected, but to the
  background at about -72 dBFS, which the model holds to be inaudible and does not code. That may
  well be right at a normal listening level, and it may matter little on real recordings.

Without the limit, the model is still behind flat noise on white noise, by 0.05 to 0.32, on the harmonic
signal at 16 kbit/s (3.75 against 4.48), and on the chords at 16 kbit/s (4.19 against 4.39). So the two
causes do not explain everything.

### Known weaknesses

- **Pre-echo.** A frame spreads its noise over its whole window. Before an attack that is clearly
  visible, and it will probably be audible. There is no block switching.

  ![Decoded signal around a burst, with noise before the attack](results/pre_echo_clicks_48k.png)

- **Attacks get too few bits.** On the chords at 48 kbit/s with the model, the frames within one frame
  of a note start are 9 % of all frames. They hold 97 % of the noise energy, and 21 % of their bands
  lie above the threshold, against 0.5 % elsewhere. In the other frames, the noise lies 29 dB below the
  threshold in the median. So bits are left over there while the attacks run short, because each frame
  has the same fixed budget. A bit reservoir could address this. These figures come from one separate
  check of the `codec-analyze` table and the decoded file; the scripts do not produce them.
- **Hardly any gain on tonal music.** On the same signal, the mean excess is about the same with the
  model and with flat noise (0.07 against 0.09 dB), slightly more bands lie above the threshold (2.3 %
  against 1.6 %), and the SNR is 36 dB lower. A sparse signal leaves flat noise enough room, so the
  listening test needs tonal items.
- **Low bitrates.** On the harmonic signal and on pink noise at 16 kbit/s, and on the harmonic signal
  at 32 kbit/s, the mean excess is lower with the model, but more bands lie above the threshold than
  with flat noise (63 % against 54 %, 78 % against 69 %, 42 % against 31 %).

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
