#pragma once

#include <cstdint>

#include "codec/types.hpp"

namespace codec {

inline constexpr int kMinGain = 0;
inline constexpr int kMaxGain = 255;
inline constexpr int kMinScalefactor = -128;
inline constexpr int kMaxScalefactor = 127;

/// Largest magnitude that quantize() returns.
inline constexpr std::int32_t kMaxQuantized = 1 << 30;

/// Quantization step of a band: 2^((gain - scalefactor - 144) / 8). One unit of gain changes the
/// step by 0.75 dB, a higher gain gives a coarser step, and a higher scalefactor a finer one.
/// Throws std::invalid_argument if gain or scalefactor are out of range.
///
/// The result comes from a table of constants instead of std::pow, so it is the same double on
/// every platform.
Sample step_size(int gain, int scalefactor);

/// Rounds value / step to the nearest integer, with ties away from zero. Magnitudes above
/// kMaxQuantized are limited to it, and NaN becomes 0.
std::int32_t quantize(Sample value, Sample step) noexcept;

Sample dequantize(std::int32_t quantized, Sample step) noexcept;

}  // namespace codec
