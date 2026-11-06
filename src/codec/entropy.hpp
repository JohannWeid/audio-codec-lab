#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "codec/range_coder.hpp"

namespace codec {

/// Maps signed values to unsigned ones, so that small magnitudes stay small:
/// 0, -1, 1, -2, 2 become 0, 1, 2, 3, 4.
std::uint32_t zigzag_encode(std::int32_t value) noexcept;
std::int32_t zigzag_decode(std::uint32_t value) noexcept;

/// Number of bits of a value, which is 0 for 0, 1 for 1, 2 for 2 and 3, 3 for 4 to 7, and so on.
int bit_length(std::uint32_t value) noexcept;

/// Adaptive probabilities for unsigned integers of any size. A value is coded by its bit length as
/// a series of decisions, followed by the bits below its leading one. The first of these is
/// adaptive as well, and the others are close enough to random to be coded directly.
struct AdaptiveValue {
    static constexpr std::size_t kModels = 16;
    std::array<AdaptiveBit, kModels> length{};
    std::array<AdaptiveBit, kModels> first_bit{};
};

void encode_value(RangeEncoder& encoder, AdaptiveValue& model, std::uint32_t value);

/// Any input gives a value below 2^32, since the bit length ends at 32.
std::uint32_t decode_value(RangeDecoder& decoder, AdaptiveValue& model) noexcept;

}  // namespace codec
