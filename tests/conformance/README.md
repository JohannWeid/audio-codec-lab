# Conformance data

These files show that a bitstream decodes to the same result on every platform, as section 4 of
[the specification](../../spec/codec-spec.md) requires. They were created once and are checked in,
so every platform in CI decodes the same bytes.

| File | Content |
|---|---|
| `input.wav` | Test signal of 0.25 seconds, made by `regenerate.py` |
| `ref_32k.bin`, `ref_64k.bin` | Bitstreams of that signal with flat noise, the baseline |
| `ref_psy_64k.bin` | Bitstream of that signal with the psychoacoustic model |
| The `.wav` file of each bitstream | Its decoded output on the platform that created it |

[`tests/test_conformance.cpp`](../test_conformance.cpp) checks three things on every platform:

1. The integers in each bitstream have the expected hash, so they are bit-exact.
2. The decoded PCM stays within the tolerance of the checked-in output.
3. The encoder reaches the same quality as the reference. Its bitstream may differ between platforms,
   and the test log reports whether it does.

Create new files only if the bitstream format or the output of the encoder changes on purpose:

```bash
python3 tests/conformance/regenerate.py build/default
```

Then run the tests and update the hashes in `tests/test_conformance.cpp`.
