#pragma once

#include <cstdint>

#include "codec/bitstream.hpp"

namespace codec {

/// Largest Rice parameter k that a code may use.
inline constexpr int kMaxRiceParameter = 31;

/// Maps signed values to unsigned ones, so that small magnitudes stay small:
/// 0, -1, 1, -2, 2 become 0, 1, 2, 3, 4.
std::uint32_t zigzag_encode(std::int32_t value) noexcept;
std::int32_t zigzag_decode(std::uint32_t value) noexcept;

/// Exponential-Golomb code of order 0. Throws std::invalid_argument for 0xFFFFFFFF, which the code
/// cannot represent.
void write_exp_golomb(BitWriter& writer, std::uint32_t value);
std::uint32_t read_exp_golomb(BitReader& reader);
int exp_golomb_bits(std::uint32_t value);

/// Rice code with parameter k: the quotient value >> k in unary, then the k lowest bits. Quotients
/// of kRiceEscape or more switch to an Exponential-Golomb code, so no code word can get extremely
/// long even if one value is far above the others. Throws std::invalid_argument if k is not in
/// 0 to kMaxRiceParameter.
void write_rice(BitWriter& writer, std::uint32_t value, int k);
std::uint32_t read_rice(BitReader& reader, int k);
int rice_bits(std::uint32_t value, int k);

/// Length of the unary part from which the Exponential-Golomb escape takes over.
inline constexpr std::uint32_t kRiceEscape = 24;

}  // namespace codec
