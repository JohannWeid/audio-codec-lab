#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "codec/bitstream.hpp"
#include "codec/entropy.hpp"

namespace {

using codec::BitReader;
using codec::BitstreamError;
using codec::BitWriter;
using codec::exp_golomb_bits;
using codec::kMaxRiceParameter;
using codec::kRiceEscape;
using codec::read_exp_golomb;
using codec::read_rice;
using codec::rice_bits;
using codec::write_exp_golomb;
using codec::write_rice;
using codec::zigzag_decode;
using codec::zigzag_encode;

constexpr std::uint32_t kMax32 = std::numeric_limits<std::uint32_t>::max();

// The bits of a writer as text, which makes the expected code words readable.
std::string bit_text(const BitWriter& writer) {
    std::string text;
    for (std::size_t i = 0; i < writer.bit_count(); ++i) {
        const std::uint8_t byte = writer.bytes()[i / 8];
        text += ((byte >> (7 - i % 8)) & 1) != 0 ? '1' : '0';
    }
    return text;
}

// A reader over bits given as text.
std::vector<std::uint8_t> bytes_from_text(const std::string& text) {
    BitWriter writer;
    for (const char c : text) {
        writer.write_bit(c == '1');
    }
    writer.align_to_byte();
    return writer.bytes();
}

std::string rice_text(std::uint32_t value, int k) {
    BitWriter writer;
    write_rice(writer, value, k);
    return bit_text(writer);
}

std::string exp_golomb_text(std::uint32_t value) {
    BitWriter writer;
    write_exp_golomb(writer, value);
    return bit_text(writer);
}

}  // namespace

TEST(Zigzag, MapsSmallMagnitudesToSmallValues) {
    EXPECT_EQ(zigzag_encode(0), 0u);
    EXPECT_EQ(zigzag_encode(-1), 1u);
    EXPECT_EQ(zigzag_encode(1), 2u);
    EXPECT_EQ(zigzag_encode(-2), 3u);
    EXPECT_EQ(zigzag_encode(2), 4u);
}

TEST(Zigzag, RoundtripsIncludingExtremes) {
    const std::int32_t lowest = std::numeric_limits<std::int32_t>::min();
    const std::int32_t highest = std::numeric_limits<std::int32_t>::max();
    EXPECT_EQ(zigzag_encode(highest), kMax32 - 1);
    EXPECT_EQ(zigzag_encode(lowest), kMax32);
    for (const std::int32_t value :
         {lowest, lowest + 1, -1000, -1, 0, 1, 1000, highest - 1, highest}) {
        EXPECT_EQ(zigzag_decode(zigzag_encode(value)), value);
    }
}

TEST(ExpGolomb, WritesExpectedCodeWords) {
    EXPECT_EQ(exp_golomb_text(0), "1");
    EXPECT_EQ(exp_golomb_text(1), "010");
    EXPECT_EQ(exp_golomb_text(2), "011");
    EXPECT_EQ(exp_golomb_text(3), "00100");
    EXPECT_EQ(exp_golomb_text(6), "00111");
    EXPECT_EQ(exp_golomb_text(7), "0001000");
}

TEST(ExpGolomb, RoundtripsAndReportsItsLength) {
    const std::uint32_t values[] = {0, 1, 2, 3, 6, 7, 254, 255, 65535, 1u << 31, kMax32 - 1};
    for (const std::uint32_t value : values) {
        SCOPED_TRACE(value);
        BitWriter writer;
        write_exp_golomb(writer, value);
        EXPECT_EQ(writer.bit_count(), static_cast<std::size_t>(exp_golomb_bits(value)));
        BitReader reader(writer.bytes().data(), writer.bytes().size());
        EXPECT_EQ(read_exp_golomb(reader), value);
    }
}

TEST(ExpGolomb, RejectsTheValueItCannotRepresent) {
    BitWriter writer;
    EXPECT_THROW(write_exp_golomb(writer, kMax32), std::invalid_argument);
    EXPECT_THROW(exp_golomb_bits(kMax32), std::invalid_argument);
}

TEST(ExpGolomb, RejectsDamagedCodes) {
    const std::vector<std::uint8_t> zeros(16, 0);
    BitReader too_many_zeros(zeros.data(), zeros.size());
    EXPECT_THROW(read_exp_golomb(too_many_zeros), BitstreamError);

    // With 32 or more leading zeros and enough data behind them, the code describes a value beyond
    // 32 bits. It must be rejected instead of being cut off silently.
    for (const std::size_t leading_zeros : {32u, 33u, 40u}) {
        SCOPED_TRACE(leading_zeros);
        const std::vector<std::uint8_t> long_code = bytes_from_text(
            std::string(leading_zeros, '0') + "1" + std::string(leading_zeros, '0'));
        BitReader long_reader(long_code.data(), long_code.size());
        EXPECT_THROW(read_exp_golomb(long_reader), BitstreamError);
    }

    // The code announces three more bits after the leading one, but the data ends before them.
    const std::vector<std::uint8_t> one_byte = {0b00000001};
    BitReader truncated(one_byte.data(), one_byte.size());
    EXPECT_THROW(read_exp_golomb(truncated), BitstreamError);
}

TEST(Rice, WritesExpectedCodeWords) {
    EXPECT_EQ(rice_text(0, 0), "0");
    EXPECT_EQ(rice_text(3, 0), "1110");
    EXPECT_EQ(rice_text(0, 2), "000");
    EXPECT_EQ(rice_text(3, 2), "011");
    EXPECT_EQ(rice_text(4, 2), "1000");
    EXPECT_EQ(rice_text(11, 2), "11011");
}

TEST(Rice, UsesEscapeForLargeQuotients) {
    const std::string unary(kRiceEscape, '1');
    // The longest regular code ends with a zero bit after 23 ones.
    EXPECT_EQ(rice_text(kRiceEscape - 1, 0), std::string(kRiceEscape - 1, '1') + "0");
    // From 24 on, the ones have no end marker and an Exponential-Golomb code follows.
    EXPECT_EQ(rice_text(kRiceEscape, 0), unary + "1");
    EXPECT_EQ(rice_text(kRiceEscape + 1, 0), unary + "010");
    // The low bits stay at the end of the code.
    EXPECT_EQ(rice_text((kRiceEscape << 2) | 0b10u, 2), unary + "1" + "10");
}

TEST(Rice, EscapeKeepsLargeValuesShort) {
    // A plain Rice code would need more than four billion bits for this value.
    EXPECT_LE(rice_bits(kMax32, 0), 24 + 63);
}

TEST(Rice, RoundtripsAndReportsItsLength) {
    const std::uint32_t values[] = {0,     1,        2,        3,          22,    23,
                                    24,    25,       100,      1023,       1024,  65535,
                                    65536, 1u << 24, 1u << 31, kMax32 - 1, kMax32};
    for (int k = 0; k <= kMaxRiceParameter; ++k) {
        for (const std::uint32_t value : values) {
            SCOPED_TRACE(testing::Message() << "k=" << k << " value=" << value);
            BitWriter writer;
            write_rice(writer, value, k);
            ASSERT_EQ(writer.bit_count(), static_cast<std::size_t>(rice_bits(value, k)));
            BitReader reader(writer.bytes().data(), writer.bytes().size());
            ASSERT_EQ(read_rice(reader, k), value);
            ASSERT_LT(reader.bits_remaining(), 8u);
        }
    }
}

TEST(Rice, RandomSequenceRoundtrips) {
    std::mt19937 rng(22);
    std::vector<std::uint32_t> values;
    std::vector<int> parameters;
    BitWriter writer;
    for (int i = 0; i < 20000; ++i) {
        const int k = static_cast<int>(rng() % 20);
        // Mixed sizes, so that regular codes and the escape both occur.
        const auto value = static_cast<std::uint32_t>(rng()) >> (rng() % 32);
        write_rice(writer, value, k);
        values.push_back(value);
        parameters.push_back(k);
    }
    BitReader reader(writer.bytes().data(), writer.bytes().size());
    for (std::size_t i = 0; i < values.size(); ++i) {
        ASSERT_EQ(read_rice(reader, parameters[i]), values[i]);
    }
}

TEST(Rice, RejectsInvalidParameters) {
    BitWriter writer;
    const std::vector<std::uint8_t> bytes(8);
    BitReader reader(bytes.data(), bytes.size());
    for (const int k : {-1, 32}) {
        SCOPED_TRACE(k);
        EXPECT_THROW(write_rice(writer, 0, k), std::invalid_argument);
        EXPECT_THROW(read_rice(reader, k), std::invalid_argument);
        EXPECT_THROW(rice_bits(0, k), std::invalid_argument);
    }
}

// With k = 31, only quotients 0 and 1 fit into 32 bits, so a longer unary part is damage. The value
// must not silently wrap around.
TEST(Rice, RejectsQuotientsThatDoNotFitIntoTheValue) {
    const std::vector<std::uint8_t> unary_too_long = bytes_from_text("110" + std::string(31, '0'));
    BitReader reader(unary_too_long.data(), unary_too_long.size());
    EXPECT_THROW(read_rice(reader, 31), BitstreamError);

    // 24 ones and then the largest escape, far beyond what k = 8 can hold.
    const std::string largest_escape = std::string(kRiceEscape, '1') + std::string(31, '0') + "1" +
                                       std::string(31, '1') + std::string(8, '0');
    const std::vector<std::uint8_t> escape_too_long = bytes_from_text(largest_escape);
    BitReader escape_reader(escape_too_long.data(), escape_too_long.size());
    EXPECT_THROW(read_rice(escape_reader, 8), BitstreamError);
}

TEST(Rice, RejectsTruncatedCodes) {
    const std::vector<std::uint8_t> only_ones(2, 0xFF);
    BitReader reader(only_ones.data(), only_ones.size());
    EXPECT_THROW(read_rice(reader, 0), BitstreamError);

    BitReader empty(nullptr, 0);
    EXPECT_THROW(read_rice(empty, 3), BitstreamError);
}

// The sanitizer job in CI turns any undefined behavior here into a failure.
TEST(EntropyRobustness, RandomBitsAreHandled) {
    std::mt19937 rng(23);
    std::vector<std::uint8_t> bytes(4096);
    for (int round = 0; round < 200; ++round) {
        for (std::uint8_t& byte : bytes) {
            byte = static_cast<std::uint8_t>(rng() & 0xFFu);
        }
        BitReader reader(bytes.data(), bytes.size());
        try {
            while (reader.bits_remaining() > 0) {
                const int k = static_cast<int>(rng() % 32);
                (void)read_rice(reader, k);
                (void)read_exp_golomb(reader);
            }
        } catch (const BitstreamError&) {
            // Rejecting is fine. Any other exception fails the test.
        }
    }
}
