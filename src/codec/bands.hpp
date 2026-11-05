#pragma once

#include <array>
#include <cstddef>

namespace codec {

/// Number of scalefactor bands for 1024 coefficients at 48 kHz.
inline constexpr std::size_t kBandCount = 25;

/// Band b covers the coefficients from kBandEdges[b] up to but not including kBandEdges[b + 1]. The
/// table and its derivation are given in section 6 of spec/codec-spec.md.
inline constexpr std::array<std::size_t, kBandCount + 1> kBandEdges = {
    0,  4,  9,   13,  18,  23,  28,  33,  39,  46,  54,  62,  72,
    84, 98, 116, 137, 163, 194, 231, 274, 325, 391, 487, 658, 1024};

}  // namespace codec
