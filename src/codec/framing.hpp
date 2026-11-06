#pragma once

#include <cstddef>
#include <vector>

#include "codec/mdct.hpp"
#include "codec/types.hpp"

namespace codec {

/// Number of frames for a signal of total_samples samples: ceil(total_samples / M) + 1.
std::size_t frame_count(std::size_t total_samples, std::size_t coefficient_count);

/// Splits a signal into overlapping blocks, following the framing rules in section 2 of
/// spec/codec-spec.md. Returns frame_count() blocks of 2M samples each.
std::vector<std::vector<Sample>> split_into_blocks(const std::vector<Sample>& signal,
                                                   std::size_t coefficient_count);

/// Splits a signal into blocks and transforms each one. Returns frame_count() frames of M
/// coefficients each.
std::vector<std::vector<Sample>> analyze(const std::vector<Sample>& signal, const Mdct& mdct);

/// Inverse of analyze(): overlap-adds the frames and removes the delay, so the result has exactly
/// total_samples samples that line up with the input. Throws std::invalid_argument if the number
/// of frames does not fit total_samples or a frame has the wrong size.
std::vector<Sample> synthesize(const std::vector<std::vector<Sample>>& frames,
                               std::size_t total_samples, const Mdct& mdct);

}  // namespace codec
