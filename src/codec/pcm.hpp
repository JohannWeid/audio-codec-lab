#pragma once

#include <cstdint>
#include <vector>

#include "codec/types.hpp"

namespace codec {

/// Converts 16-bit PCM to samples by dividing by 32768, so the range is [-1, 1).
std::vector<Sample> pcm_to_samples(const std::vector<std::int16_t>& pcm);

/// Converts samples back by multiplying by 32768, rounding to the nearest integer with ties away
/// from zero, and clipping to the 16-bit range. NaN becomes 0.
std::vector<std::int16_t> samples_to_pcm(const std::vector<Sample>& samples);

}  // namespace codec
