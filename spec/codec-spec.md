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
payload ends at a byte boundary, with zero bits as padding:

| Field | Size | Meaning |
|---|---|---|
| gain | 8 bits | See section 6 |
| for each of the 25 bands: | | |
| empty flag | 1 bit | 1 if every coefficient of the band is zero; the band then ends here |
| scalefactor difference | Exponential-Golomb, zigzag | Relative to the last transmitted scalefactor, starting at 0 |
| Rice parameter difference | Exponential-Golomb, zigzag | Relative to the last transmitted parameter, starting at 0; the result is in 0 to 24 |
| coefficients | Rice, zigzag | All coefficients of the band |

A frame without any nonzero coefficient takes 7 bytes. The encoder chooses the Rice parameter of a
band that gives the fewest bits.

**Limit of the Rice code:** every band that is not empty costs at least one bit per coefficient, so
below about one bit per sample a dense signal is reduced to its strongest parts. In a measurement
with white noise, the SNR stays near 0 dB up to 48 kbit/s and reaches 2 dB at 64 kbit/s. Tonal
signals are hardly affected. This is the main reason to replace the Rice code in phase 3.

**Rice code with parameter k:** the quotient `value >> k` in unary as ones with a closing zero, then
the k lowest bits. Quotients of 24 or more are written as 24 ones without a closing zero, followed
by an Exponential-Golomb code of order 0 for `quotient - 24`, and then the k lowest bits. One value
far above the others in its band therefore cannot produce an extremely long code. Signed values go
through a zigzag mapping first: 0, -1, 1, -2, 2 become 0, 1, 2, 3, 4.

**Alignment:** the decoder drops the first M output samples and writes exactly `total samples`
samples, so that its output lines up with the input sample by sample. SNR, ViSQOL and the listening
test all depend on this.

**Damaged input:** the decoder must never crash or read out of bounds. It rejects a bitstream with a
wrong magic, an unknown version, an impossible field or a length beyond the end of the data.

## 4. Determinism

Floating-point results can differ slightly between compilers and platforms. One such difference in
the encoder can flip a decision of the rate loop and so change the whole bitstream. The tests
therefore separate what must be identical from what only has to be close:

| What | Requirement | Applies to |
|---|---|---|
| Decoded integers of a reference bitstream (scalefactors, quantized coefficients) | Bit-exact | All platforms |
| PCM output of the decoder for a reference bitstream | Within a tolerance, assumed +/-1 LSB until measured | All platforms |
| Bitstream produced by the encoder | Identical across repeated runs | Same platform and build only |

Reference bitstreams are created once and checked in under `tests/conformance`, and every platform
decodes the same files. The CI log of each platform reports the deviation that really occurs.

Build rules: no `-ffast-math`, `-ffp-contract=off` on GCC and Clang, `/fp:precise` on MSVC.
`tests/test_floating_point.cpp` guards the rule against fused multiply-add.

## 5. Psychoacoustic model

Simplified model per block: FFT with a Hann window, energy per Bark band, spreading function,
absolute threshold of hearing as a lower limit, and from these the masking threshold and the
signal-to-mask ratio per scalefactor band.

The absolute threshold is given in dB SPL, but a digital signal has no absolute level, so the model
needs an assumption: a full-scale sine corresponds to 96 dB SPL.

The FFT window must be aligned in time with the MDCT window of the same frame. The threshold comes
from the FFT and the noise is measured on MDCT coefficients, so the two energy scales must be
calibrated against each other, for example with a full-scale sine.

## 6. Bit allocation

- Outer loop: adjust the scalefactors until the noise is below the masking threshold.
- Inner loop: adjust the global step size until the frame fits the bit budget.

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

**Rate loop in phase 2:** all scalefactors are 0, and a binary search finds the smallest gain whose
frame fits the budget. This is also the flat noise allocation of the baseline in section 7.

## 7. Baseline

`--psy off` selects flat noise allocation instead of the masking model. Everything else stays the
same: transform, entropy coder and rate loop. The comparison then measures only the psychoacoustic
model. The baseline is expected to reach a similar or better SNR at the same bitrate and to sound
worse, because SNR does not reflect perception.

## 8. Open questions

- [ ] Final target bitrates, after a short listening check (phase 3)
- [ ] Coded bandwidth per bitrate: 32 kbit/s is only 0.67 bit per sample, so it may not be able to
      cover the full 24 kHz (phase 3)
- [ ] Entropy coder for phase 3: Huffman or adaptive range coder, decided from measurements
- [ ] Tolerance for the PCM comparison, confirmed by measurement on all platforms (phase 2)
- [ ] Bit reservoir as an optional extension, if strict budgets audibly hurt transients
