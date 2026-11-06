# Codec Specification (draft)

This draft records the decisions made so far. Section 8 lists what is still open, and values may
change once measurements exist.

## 1. Scope

- Input and output: WAV, 16-bit PCM, 48 kHz, mono. Other formats are rejected with an error.
- Stereo is out of scope until the mono codec is finished.
- Target bitrates: 32, 48 and 64 kbit/s, provisional until a short listening check. Constant bitrate
  with a small tolerance.
- Sample type in the signal path: `double`, through one alias (`Sample`), so that it can be changed
  in one place.
- PCM becomes a sample by dividing by 32768, so the range is [-1, 1). The way back multiplies by
  32768, rounds to the nearest integer with ties away from zero, and clips to the 16-bit range.

## 2. Transform

MDCT with M = 1024 new samples per frame, so a window of 2M = 2048 samples with 50 % overlap. The
window is a sine window; a KBD window is an optional extension.

```text
w[n] = sin(pi * (n + 1/2) / (2M))

X[k] = sqrt(2/M) * sum_{n=0}^{2M-1} w[n] * x[n] * cos(pi/M * (n + 1/2 + M/2) * (k + 1/2))
y[n] = sqrt(2/M) * w[n] * sum_{k=0}^{M-1} X[k] * cos(pi/M * (n + 1/2 + M/2) * (k + 1/2))
```

Overlap-adding `y` of consecutive frames reconstructs the signal. Unit tests check this and the
window condition `w[n]^2 + w[n + M]^2 = 1`.

The factor `sqrt(2/M)` in both directions makes the transform orthonormal: the energy of the signal
and of the quantization noise is the same in the time domain and in the coefficients. The rate loop
can therefore measure the noise per band directly on the coefficients, without an inverse transform.

**Framing:** for a signal of L samples, the encoder prepends M zeros and pads the end with zeros,
which gives `ceil(L / M) + 1` frames. Frame `i` covers the samples `i * M` to `i * M + 2M - 1` of the
padded signal.

## 3. Bitstream

All multi-byte fields are big-endian, and bits are packed starting with the most significant bit.

Header:

| Field | Size | Meaning |
|---|---|---|
| magic | 4 bytes | Identifies the format |
| version | 1 byte | Format version; a decoder rejects versions it does not know |
| sample rate | 4 bytes | In Hz |
| channels | 1 byte | 1 for now |
| frame length | 2 bytes | New samples per frame, 1024 for now |
| total samples | 8 bytes | Length of the original signal in samples |

The decoder rejects any other sample rate, channel count or frame length.

Each frame starts with its payload length in bytes as a 2-byte field, followed by the payload. The
payload is one run of a binary range coder, in this order:

| Field | How it is coded | Meaning |
|---|---|---|
| frame empty | Fixed probability of 1/16 | 1 if every coefficient is zero; the frame then ends here and keeps the gain and scalefactors of the frame before |
| gain change | Adaptive value, zigzag | Relative to the frame before, which starts at 144 |
| for each of the 25 bands: | | |
| band empty | Adaptive bit | 1 if every coefficient of the band is zero; the band then ends here |
| scalefactor change | Adaptive value, zigzag | Relative to the same band in the frame before, which starts at 0 |
| coefficients | Adaptive value for the magnitude, one direct bit for the sign if it is not 0 | All coefficients of the band |

**Range coder:** in the style of the one in LZMA, with a range of 32 bits and probabilities of 12
bits. The first byte, which is always 0, is left out. At the end, the coder picks the value in its
final range with the most trailing zero bytes and leaves those out, since the decoder reads zeros
past the end. So a payload never ends with a zero byte. A range coder can spend far less than one bit
on a likely decision and needs no tables trained on audio. It uses only integers, so it is bit-exact
on every platform.

**Adaptive bit:** the probability is the mean of two estimates of 16 bits, one moving by 1/16 and the
other by 1/128 of the distance towards each new decision. Encoder and decoder start at 1/2 and keep
the state from frame to frame, so a frame can only be decoded after all the frames before it.

**Adaptive value:** an unsigned value is coded by its bit length, as a series of adaptive bits. The
bit below the leading one is an adaptive bit too, and the bits below that are direct bits. There are
16 adaptive bits for the bit length and 16 for that one bit, chosen by position.

**Which adaptive value a coefficient uses:** one of 14, chosen by the bit lengths of its neighbors:
`2 * a + b + 2 * c + d + e`, where a and b are the two coefficients below it in this frame, and c, d
and e are the same coefficient and its two neighbors in the frame before. The bit lengths from the
frame before are corrected for a change of the step: 1 for every 8 units, rounded towards 0, with a
bit length of 0 staying 0. The sum selects a class with these upper limits: 0, 1, 2, 3, 4, 6, 8, 11,
15, 20, 27, 36, 50, and one class above that.

**Band empty flags** use one of 4 adaptive bits per band, chosen by whether the band was empty in the
frame before and whether the band below it is empty in this frame. **Scalefactor changes** use one of
2 adaptive values, chosen by whether the last change in this frame was 0. Signed values go through a
zigzag mapping: 0, -1, 1, -2, 2 become 0, 1, 2, 3, 4.

**Why a range coder and not Huffman codes:** the Rice codes of the first version needed at least one
bit per coefficient in every band that was not empty, so below about one bit per sample, a dense
signal was reduced to its strongest parts. Huffman codes have the same limit unless they code groups
of values, and those tables would have to be trained on audio. At the same bitrate, the range coder
gives a higher SNR with flat noise: 45.5 instead of 42.9 dB for harmonic tones at 32 kbit/s, 10.0
instead of 6.8 dB for pink noise at 64 kbit/s, and 4.7 instead of 2.2 dB for white noise at 64 kbit/s.

**Alignment:** the decoder drops the first M output samples and writes exactly `total samples`
samples, so that its output lines up with the input sample by sample. SNR, ViSQOL and the listening
test all depend on this.

**Damaged input:** the decoder must never crash or read out of bounds. It rejects a bitstream with a
wrong magic, an unknown version, an impossible field, a value out of range, a length beyond the end
of the data, a payload that ends with a zero byte, or a payload of which it has not read every byte.
Unlike a checksum, this cannot detect every change: changed bytes inside a payload may simply decode
to other values.

## 4. Determinism

Floating-point results can differ slightly between compilers and platforms. One such difference in
the encoder can flip a decision of the rate loop and so change the whole bitstream. The tests
therefore separate what must be identical from what only has to be close:

| What | Requirement | Applies to |
|---|---|---|
| Decoded integers of a reference bitstream (scalefactors, quantized coefficients) | Bit-exact | All platforms |
| PCM output of the decoder for a reference bitstream | Within +/-1 LSB | All platforms |
| Bitstream produced by the encoder | Identical across repeated runs | Same platform and build only |

Reference bitstreams are created once and checked in under `tests/conformance`, and every platform
decodes the same files. The CI log of each platform reports the deviation that really occurs.

**Measured on 2026-11-05**, with two reference bitstreams of 0.25 seconds each: GCC 13 and Clang 18 on
Linux x86-64, MSVC 19.51 on Windows x86-64 and AppleClang 17 on macOS ARM all decode them with a
deviation of 0 LSB, and their encoders produce bitstreams identical to the reference. The tolerance
stays at +/-1 LSB and the encoder requirement stays as above, because this is a small sample and the
psychoacoustic model of phase 3 will add more floating-point decisions to the encoder.

Build rules: no `-ffast-math`, `-ffp-contract=off` on GCC and Clang, `/fp:precise` on MSVC.
`tests/test_floating_point.cpp` guards the rule against fused multiply-add.

## 5. Psychoacoustic model

The model is simplified. For each frame, it works on the same 2048 samples as the MDCT, so both are
aligned in time:

1. **Spectrum:** an FFT with a Hann window. Its bin k stands for MDCT coefficient k.
2. **Energy scale:** the power of each bin is multiplied by `8 / (3 * 2048)`. This puts it on the
   scale of the orthonormal MDCT, so a threshold can be compared directly with the noise measured on
   the coefficients. A test checks that both agree within 1 dB per band.
3. **Band energy:** the sum over the bins of each scalefactor band.
4. **Tonality per band:** the share of the band energy in tonal peaks, including the bin on each side
   of a peak. A bin is a tonal peak if it is a local maximum and at least 7 dB above the bins around
   it. These bins start at a distance of 2 and go up to 2, 6, 12 or 24 bins, with the limit growing
   at bins 126, 254 and 510. This follows the MPEG-1 model, with its ranges doubled for this block
   length.
5. **Masking threshold:** each band acts as a masker for all bands:

   ```text
   threshold[b] = sum_j energy[j] * spread(z[b] - z[j]) * 10^(-offset[j] / 10)
   offset[j]    = tonality[j] * (14.5 + z[j]) + (1 - tonality[j]) * 5.5     (in dB, after Johnston)
   spread(x)    = 15.81 + 7.5 * (x + 0.474) - 17.5 * sqrt(1 + (x + 0.474)^2) (in dB, after Schroeder)
   ```

   `z` is the Bark value of the band center. For each masked band, the values of `spread` are
   normalized to a sum of 1, so that spreading does not raise a signal with equal energy in all
   bands. This lowers the threshold next to a single masker by a few dB, which errs on the side of
   less noise.
6. **Threshold in quiet:** no threshold falls below the absolute threshold of hearing, after Terhardt,
   at the most sensitive frequency of the band. It is given in dB SPL, but a digital signal has no
   absolute level, so the model needs an assumption: a full-scale sine corresponds to 96 dB SPL, which
   has an energy of 512 per frame in the MDCT.

The signal-to-mask ratio of a band is `energy[b] / threshold[b]`. Neither pre-masking nor post-masking
is modeled, and each frame is analyzed on its own. The real level of playback is unknown as well, so
the threshold in quiet is only an estimate.

## 6. Bit allocation

- The scalefactors follow directly from the masking thresholds.
- A rate loop adjusts the gain, and so all steps together, until the frame fits the bit budget.

Allocation sits behind one interface, so that it can be replaced as a whole.

**Quantization:** the step of a band is `2^((gain - scalefactor - 144) / 8)`, with a gain from 0 to
255 and a scalefactor from -128 to 127. One unit changes the step by 0.75 dB. A coefficient becomes
`value / step`, rounded to the nearest integer with ties away from zero. The decoder multiplies by
the step again. The steps come from a table of constants, so they are bit-exact on every platform.

**Scalefactor bands:** 25 bands, one per Bark. The Bark value of each coefficient is taken at its
center frequency with `z(f) = 13 * atan(0.00076 * f) + 3.5 * atan((f / 7500)^2)`, and a band has at
least 4 coefficients. At 48 kHz with M = 1024, one coefficient covers 23.4375 Hz. The band edges,
where band `b` covers the coefficients from `edge[b]` up to but not including `edge[b + 1]`, are:

```text
0, 4, 9, 13, 18, 23, 28, 33, 39, 46, 54, 62, 72, 84, 98, 116, 137, 163, 194, 231, 274, 325, 391,
487, 658, 1024
```

**Bit budget:** each frame has a strict budget, without a bit reservoir for now. The average budget
is often not a whole number, so frame `n` gets the following number of bits, computed with 64-bit
integers, where R is the bitrate and fs the sample rate:

```text
budget(n) = floor((n + 1) * R * M / fs) - floor(n * R * M / fs)
```

At 32 kbit/s this gives 682, 683, 683 and then repeats; at 48 kbit/s every frame gets 1024 bits. The
budget covers the whole frame including its length field. The file header is not counted. Bitrates
from 8 to 512 kbit/s are accepted, and bits that a frame leaves unused are lost.

**Rate loop:** a binary search finds the smallest gain whose frame fits the budget, for fixed
scalefactors. If not even the highest gain fits, the frame stays empty.

**Scalefactors from the masking threshold:** a quantizer with step `s` adds a noise energy of about
`s^2 / 12` per coefficient, so a band of width `w` can use the step `s = sqrt(12 * threshold / w)`.
The scalefactor is `-8 * log2(s)`, rounded. At a gain of 144, each band then has about as much noise
as its threshold allows. The rate loop moves all bands together: a lower gain stays below the
thresholds, and a higher one goes above them if the budget requires it.

**Inaudible bands stay empty:** if the whole energy of a band lies below the threshold in quiet, its
coefficients are set to zero before quantization, since nobody can hear them in any case. This keeps
a large budget from going to content such as the rounding noise of the 16-bit input. Bands that are
only masked by the rest of the signal stay: their coarse steps remove them if bits are short, and
they are coded if the budget allows it. Dropping those as well would stop the quality of a tone from
rising at about 31 dB SNR, whatever the bitrate, because the skirts of its spectrum would be missing.

**Bandwidth:** only the lowest bands that leave at least 1.75 bits per coefficient are coded, and the
ones above them stay empty. This gives 3.8 kHz at 16 kbit/s, 7.6 kHz at 32 kbit/s, 11.4 kHz at 48
kbit/s, 15.4 kHz at 64 kbit/s, and the full bandwidth from 96 kbit/s on. Each band counts about the
same for the ear, but the high ones hold far more coefficients, so with too few bits it is better to
serve the low bands properly. The value comes from a comparison of 1, 1.25, 1.5, 1.75, 2 and 3 bits
on tones, harmonic tones, pink noise and white noise, by the mean excess alone. Without this limit, the
model does worse than flat noise by that measure for broadband signals at low bitrates. ViSQOL says the
opposite: the limit is the main reason why it prefers flat noise, see the
[evaluation](../eval/README.md#visqol-disagrees). The bandwidth stays the same from frame to frame,
which avoids the audible flicker that a limit changing with the signal can cause. It applies to the
model only, not to the baseline.

**Mean excess:** the measure used in this document. It is the amount in dB by which noise lies above
the thresholds, averaged over all bands, with bands below their threshold counting as 0.

**No outer loop:** classic encoders use a second loop that makes the steps finer in each band that is
still above its threshold. An earlier version had this, with up to 8 rounds of 1.5 dB each. Across
budgets of 40 to 168 bytes per frame, it changed the mean excess only once, by 0.001 dB, so it was
removed. The scalefactors already follow from the thresholds, so there is little left to correct: if
bits are short, the loop cannot create any, and if they are enough, the noise is below the thresholds
anyway.

## 7. Baseline

`--psy off` selects flat noise allocation instead of the masking model: all scalefactors are 0, with
no limit on the bandwidth. Everything else stays the same: transform, entropy coder and rate loop. The
comparison then measures only the psychoacoustic model. The baseline is expected to reach a similar
or better SNR at the same bitrate and to sound worse, because SNR does not reflect perception.

Measured as the mean excess over the model's own thresholds, flat noise against the model, in dB:

| Signal | 16 kbit/s | 32 kbit/s | 48 kbit/s | 64 kbit/s | 96 kbit/s |
|---|---|---|---|---|---|
| Tone at 1 kHz | 0.08 / 0.00 | 0.00 / 0.00 | 0.00 / 0.00 | 0.00 / 0.00 | 0.00 / 0.00 |
| Harmonic tones with weak noise | 1.10 / 2.20 | 0.86 / 0.65 | 0.60 / 0.22 | 0.37 / 0.00 | 0.02 / 0.00 |
| Pink noise | 3.15 / 2.80 | 1.87 / 1.01 | 1.03 / 0.48 | 0.52 / 0.25 | 0.04 / 0.00 |
| White noise | 6.02 / 3.19 | 5.18 / 1.09 | 3.92 / 0.49 | 2.19 / 0.26 | 0.31 / 0.01 |

From 32 kbit/s on, the model is never worse than flat noise here, and from 96 kbit/s on it keeps
practically all noise below its thresholds. Below that, a part of the excess is the limited bandwidth.
At 16 kbit/s,
the harmonic tones do worse, because the bandwidth of 3.8 kHz removes their tone at 5.3 kHz. The SNR
falls as expected, for example from 45.5 to 25.2 dB for the harmonic tones at 32 kbit/s. These signals
are synthetic, and these values only compare against the model itself, so whether it sounds better
can only be shown by a listening test with real audio. The [evaluation](../eval/README.md) measures
the decoded files on more signals, including its limits.

## 8. Open questions

- [ ] Final target bitrates, after a short listening check
- [ ] Whether the bandwidth limit stays. The mean excess and ViSQOL disagree on it, so this also takes
      listening and real music.
- [ ] Bit reservoir as an optional extension, if strict budgets audibly hurt transients. The
      [evaluation](../eval/README.md) shows that attacks do run short of bits while other frames have
      bits to spare; whether this is audible is still open.
