# Perceptual Audio Codec in C++

[![CI](https://github.com/JohannWeid/audio-codec-lab/actions/workflows/ci.yml/badge.svg)](https://github.com/JohannWeid/audio-codec-lab/actions/workflows/ci.yml)

A small MDCT-based perceptual transform codec with a psychoacoustic model, a rate loop and entropy
coding, tested on Windows, Linux and macOS and evaluated with a MUSHRA-style listening test. It is a
learning project about the principles of perceptual coding, not a competitor to AAC or Opus.

**Status: work in progress.** The encoder and decoder work end to end, from WAV to bitstream and
back, but still without a psychoacoustic model: the quantization noise is flat, so the codec is not
yet perceptual. There are no listening results to show.

| Phase | Content | Status |
|---|---|---|
| 1 | Specification | in progress |
| 2 | Core: WAV, MDCT, quantization, bitstream, encoder and decoder without psychoacoustics | done, with Rice codes as a placeholder |
| 3 | Psychoacoustic model, bit allocation with rate loop, better entropy coding | planned |
| 4 | Test levels: unit, roundtrip, regression, end-to-end, fuzzing | CI skeleton only |
| 5 | Evaluation: objective metrics and a MUSHRA-style listening test | planned |

## Method (planned)

```text
Encoder
  PCM --> window + MDCT --> quantization --> entropy coding --> bitstream
   |                             ^
   +--> FFT --> masking model --> bit allocation with rate loop

Decoder
  bitstream --> entropy decoding --> dequantization --> IMDCT + overlap-add --> PCM
```

The noise from quantization is shaped so that it stays below the masking threshold of the signal.
A baseline with the same transform, entropy coder and rate loop but without the psychoacoustic model
shows what the model contributes. The details are in the
[draft specification](spec/codec-spec.md).

## Build and test

You need CMake 3.25 or newer, a C++17 compiler and network access for the first configure, which
downloads GoogleTest.

```bash
cmake --preset default
cmake --build --preset default
ctest --preset default
```

| Preset | Purpose |
|---|---|
| `default` | Release build |
| `ci` | Release build with warnings as errors, used by CI |
| `sanitize` | Debug build with AddressSanitizer and UndefinedBehaviorSanitizer, GCC and Clang only |

## Usage

The input must be a WAV file with 16-bit PCM, 48 kHz and one channel. The tools are in
`build/default`, or in `build/default/Release` on Windows.

```bash
codec-encode in.wav out.bin --bitrate 64000
codec-decode out.bin out.wav
```

## Repository layout

```text
spec/               Codec and bitstream specification
src/codec/          Codec library
src/tools/          Command-line tools codec-encode and codec-decode
tests/              Unit tests with GoogleTest
tests/e2e/          End-to-end tests with Robot Framework (planned)
eval/               Metrics, plots and listening-test analysis in Python (planned)
listening-test/     Listening-test setup, without licensed audio files
.github/workflows/  CI: build matrix, sanitizers, formatting
```

## License

The code is under the [MIT License](LICENSE). Audio files used for the evaluation are not part of
this repository; their sources and licenses will be documented in
[listening-test/README.md](listening-test/README.md).
