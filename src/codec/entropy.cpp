#include "codec/entropy.hpp"

#include <limits>
#include <stdexcept>

namespace codec {
namespace {

constexpr std::uint32_t kMaxExpGolomb = std::numeric_limits<std::uint32_t>::max() - 1;
constexpr int kMaxExpGolombPrefix = 31;

void check_rice_parameter(int k) {
    if (k < 0 || k > kMaxRiceParameter) {
        throw std::invalid_argument("the Rice parameter must be in 0 to 31");
    }
}

// Number of bits of value + 1, which is at least 1.
int code_length(std::uint32_t value) noexcept {
    std::uint64_t remaining = std::uint64_t{value} + 1;
    int length = 0;
    while (remaining != 0) {
        remaining >>= 1;
        ++length;
    }
    return length;
}

}  // namespace

std::uint32_t zigzag_encode(std::int32_t value) noexcept {
    // Written out, because shifting negative values is not fully portable in C++17. The negative
    // branch works for the smallest value too, since value + 1 cannot overflow there.
    return value >= 0 ? 2u * static_cast<std::uint32_t>(value)
                      : 2u * static_cast<std::uint32_t>(-(value + 1)) + 1u;
}

std::int32_t zigzag_decode(std::uint32_t value) noexcept {
    const auto half = static_cast<std::int32_t>(value >> 1);
    return (value & 1u) != 0 ? -1 - half : half;
}

void write_exp_golomb(BitWriter& writer, std::uint32_t value) {
    if (value > kMaxExpGolomb) {
        throw std::invalid_argument("the value is too large for the Exponential-Golomb code");
    }
    const int length = code_length(value);
    writer.write_bits(0, length - 1);
    writer.write_bits(value + 1, length);
}

std::uint32_t read_exp_golomb(BitReader& reader) {
    int prefix = 0;
    while (!reader.read_bit()) {
        ++prefix;
        if (prefix > kMaxExpGolombPrefix) {
            throw BitstreamError("an Exponential-Golomb code has too many leading zeros");
        }
    }
    // With at most 31 leading zeros the code is below 2^32, so the result always fits.
    const std::uint64_t coded = (std::uint64_t{1} << prefix) | reader.read_bits(prefix);
    return static_cast<std::uint32_t>(coded - 1);
}

int exp_golomb_bits(std::uint32_t value) {
    if (value > kMaxExpGolomb) {
        throw std::invalid_argument("the value is too large for the Exponential-Golomb code");
    }
    return 2 * code_length(value) - 1;
}

void write_rice(BitWriter& writer, std::uint32_t value, int k) {
    check_rice_parameter(k);
    const std::uint32_t quotient = value >> k;
    if (quotient < kRiceEscape) {
        for (std::uint32_t i = 0; i < quotient; ++i) {
            writer.write_bit(true);
        }
        writer.write_bit(false);
    } else {
        for (std::uint32_t i = 0; i < kRiceEscape; ++i) {
            writer.write_bit(true);
        }
        write_exp_golomb(writer, quotient - kRiceEscape);
    }
    const std::uint64_t low_mask = (std::uint64_t{1} << k) - 1;
    writer.write_bits(static_cast<std::uint32_t>(value & low_mask), k);
}

std::uint32_t read_rice(BitReader& reader, int k) {
    check_rice_parameter(k);
    std::uint64_t quotient = 0;
    while (quotient < kRiceEscape && reader.read_bit()) {
        ++quotient;
    }
    if (quotient == kRiceEscape) {
        quotient += read_exp_golomb(reader);
    }
    // A damaged code can describe more than 32 bits, in the unary part as well if k is large.
    if (quotient > (std::numeric_limits<std::uint32_t>::max() >> k)) {
        throw BitstreamError("a Rice code is out of range");
    }
    const std::uint32_t low_bits = reader.read_bits(k);
    return static_cast<std::uint32_t>((quotient << k) | low_bits);
}

int rice_bits(std::uint32_t value, int k) {
    check_rice_parameter(k);
    const std::uint32_t quotient = value >> k;
    const int prefix = quotient < kRiceEscape ? static_cast<int>(quotient) + 1
                                              : static_cast<int>(kRiceEscape) +
                                                    exp_golomb_bits(quotient - kRiceEscape);
    return prefix + k;
}

}  // namespace codec
