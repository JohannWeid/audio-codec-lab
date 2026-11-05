# Codec Specification (draft)

This draft is not final. Values marked **proposal** still need a decision, and section 8 lists the
open questions.

## 1. Scope

- Input and output: WAV, 16-bit PCM, 48 kHz, mono. Other formats are rejected with an error.
- Stereo is out of scope until the mono codec is finished.
- Target bitrates: **proposal** 32, 48 and 64 kbit/s, constant bitrate with a small tolerance.

## 2. Transform

- MDCT with 1024 new samples per frame, so a window of 2048 samples with 50 % overlap.
- Sine window first. A KBD window is an optional extension.
- The window must satisfy `w[n]^2 + w[n + 1024]^2 = 1`, which a unit test checks.

## 3. Bitstream

All multi-byte fields are big-endian (**proposal**), and bits are packed starting with the most
significant bit.

Header:

| Field | Size | Meaning |
|---|---|---|
| magic | 4 bytes | Identifies the format |
| version | 1 byte | Format version; a decoder rejects versions it does not know |
| sample rate | 4 bytes | In Hz |
| channels | 1 byte | 1 for now |
| frame length | 2 bytes | New samples per frame, 1024 for now |
| total samples | 8 bytes | Length of the original signal in samples |

Each frame starts with its payload length in bytes, followed by the payload. The payload layout
depends on the entropy coder and is defined in phase 2.

**Alignment:** the decoder removes the encoder delay and writes exactly `total samples` samples, so
that its output lines up with the input sample by sample. SNR, ViSQOL and the listening test all
depend on this.

**Damaged input:** the decoder must never crash or read out of bounds. It rejects a bitstream with a
wrong magic, an unknown version, an impossible field or a length beyond the end of the data.

## 4. Determinism

Floating-point results can differ slightly between compilers and platforms. One such difference in
the encoder can flip a decision of the rate loop and so change the whole bitstream. The tests
therefore separate what must be identical from what only has to be close:

| What | Requirement | Applies to |
|---|---|---|
| Decoded integers of a reference bitstream (scalefactors, quantized coefficients) | Bit-exact | All platforms |
| PCM output of the decoder for a reference bitstream | Within a tolerance, **proposal** +/-1 LSB | All platforms |
| Bitstream produced by the encoder | Identical across repeated runs | Same platform and build only |

Reference bitstreams are created once and checked in, and every platform decodes the same files.

Build rules: no `-ffast-math`, `-ffp-contract=off` on GCC and Clang, `/fp:precise` on MSVC.
`tests/test_floating_point.cpp` guards the rule against fused multiply-add.

## 5. Psychoacoustic model

Simplified model per block: FFT with a Hann window, energy per Bark band, spreading function,
absolute threshold of hearing as a lower limit, and from these the masking threshold and the
signal-to-mask ratio per scalefactor band.

The absolute threshold is given in dB SPL, but a digital signal has no absolute level, so the model
needs an assumption. **Proposal:** a full-scale sine corresponds to 96 dB SPL.

The FFT window must be aligned in time with the MDCT window of the same frame.

## 6. Bit allocation

- Outer loop: adjust the scalefactors until the noise is below the masking threshold.
- Inner loop: adjust the global step size until the frame fits the bit budget.

Allocation sits behind one interface, so that it can be replaced as a whole.

## 7. Baseline

`--psy off` (**proposal**) selects flat noise allocation instead of the masking model. Everything
else stays the same: transform, entropy coder and rate loop. The comparison then measures only the
psychoacoustic model. The baseline is expected to reach a similar or better SNR at the same bitrate
and to sound worse, because SNR does not reflect perception.

## 8. Open questions

- [ ] MDCT scaling convention, and where the normalization sits (forward or inverse)
- [ ] Sample type in the signal path: `float` or `double`
- [ ] Target bitrates: at which bitrates are differences audible, without everything being equally bad?
- [ ] Scalefactor band table for 1024 coefficients at 48 kHz
- [ ] Bit budget per frame: strict, or with a bit reservoir
- [ ] Entropy coder for phase 3: Huffman or adaptive range coder
- [ ] Tolerance for the PCM comparison, confirmed by measurement on all platforms
