#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <stdexcept>
#include <vector>

#include "codec/bitstream.hpp"

namespace {

using Bytes = std::vector<std::uint8_t>;
using codec::BitReader;
using codec::BitstreamError;
using codec::BitWriter;

}  // namespace

TEST(BitWriter, PacksMostSignificantBitFirst) {
    BitWriter writer;
    writer.write_bits(0b101, 3);
    writer.write_bits(0b00001, 5);
    writer.write_bits(0b11, 2);
    EXPECT_EQ(writer.bit_count(), 10u);
    EXPECT_EQ(writer.bytes(), (Bytes{0b10100001, 0b11000000}));
}

TEST(BitWriter, AlignFillsWithZerosOnlyIfNeeded) {
    BitWriter writer;
    writer.align_to_byte();
    EXPECT_EQ(writer.bit_count(), 0u);

    writer.write_bit(true);
    writer.align_to_byte();
    EXPECT_EQ(writer.bit_count(), 8u);
    writer.align_to_byte();
    EXPECT_EQ(writer.bit_count(), 8u);
    EXPECT_EQ(writer.bytes(), (Bytes{0x80}));
}

TEST(BitWriter, HandlesZeroAndFullWidth) {
    BitWriter writer;
    writer.write_bits(0, 0);
    EXPECT_EQ(writer.bit_count(), 0u);
    writer.write_bits(0xDEADBEEFu, 32);
    EXPECT_EQ(writer.bytes(), (Bytes{0xDE, 0xAD, 0xBE, 0xEF}));
}

TEST(BitWriter, RejectsInvalidArguments) {
    BitWriter writer;
    EXPECT_THROW(writer.write_bits(0, -1), std::invalid_argument);
    EXPECT_THROW(writer.write_bits(0, 33), std::invalid_argument);
    EXPECT_THROW(writer.write_bits(4, 2), std::invalid_argument);
    EXPECT_THROW(writer.write_bits(1, 0), std::invalid_argument);
    EXPECT_EQ(writer.bit_count(), 0u);
}

TEST(BitReader, ReadsBitsInWriterOrder) {
    const Bytes bytes = {0b10100001, 0b11000000};
    BitReader reader(bytes.data(), bytes.size());
    EXPECT_EQ(reader.bits_remaining(), 16u);
    EXPECT_EQ(reader.read_bits(3), 0b101u);
    EXPECT_EQ(reader.read_bits(5), 0b00001u);
    EXPECT_TRUE(reader.read_bit());
    EXPECT_TRUE(reader.read_bit());
    EXPECT_FALSE(reader.read_bit());
    EXPECT_EQ(reader.bits_remaining(), 5u);
}

TEST(BitReader, ThrowsAtTheEnd) {
    const Bytes bytes = {0xFF};
    BitReader reader(bytes.data(), bytes.size());
    EXPECT_EQ(reader.read_bits(8), 0xFFu);
    EXPECT_THROW(reader.read_bit(), BitstreamError);
    EXPECT_THROW(reader.read_bits(1), BitstreamError);

    BitReader short_reader(bytes.data(), bytes.size());
    EXPECT_THROW(short_reader.read_bits(9), BitstreamError);

    BitReader empty(nullptr, 0);
    EXPECT_EQ(empty.bits_remaining(), 0u);
    EXPECT_EQ(empty.read_bits(0), 0u);
    EXPECT_THROW(empty.read_bit(), BitstreamError);
}

TEST(BitReader, RejectsInvalidCounts) {
    const Bytes bytes(16);
    BitReader reader(bytes.data(), bytes.size());
    EXPECT_THROW(reader.read_bits(-1), std::invalid_argument);
    EXPECT_THROW(reader.read_bits(33), std::invalid_argument);
}

TEST(BitReader, ReadsFullWidth) {
    const Bytes bytes = {0xDE, 0xAD, 0xBE, 0xEF};
    BitReader reader(bytes.data(), bytes.size());
    EXPECT_EQ(reader.read_bits(32), 0xDEADBEEFu);
}

TEST(BitReader, SkipsZeroPadding) {
    const Bytes bytes = {0b10000000, 0xAB};
    BitReader reader(bytes.data(), bytes.size());
    reader.skip_padding();  // Already at a boundary, so nothing is skipped.
    EXPECT_EQ(reader.bits_remaining(), 16u);
    EXPECT_TRUE(reader.read_bit());
    reader.skip_padding();
    EXPECT_EQ(reader.read_bits(8), 0xABu);
}

TEST(BitReader, RejectsPaddingThatIsNotZero) {
    const Bytes bytes = {0b10000001};
    BitReader reader(bytes.data(), bytes.size());
    EXPECT_TRUE(reader.read_bit());
    EXPECT_THROW(reader.skip_padding(), BitstreamError);
}

TEST(Bitstream, RandomFieldsRoundtrip) {
    std::mt19937 rng(21);
    struct Field {
        std::uint32_t value;
        int count;
    };
    std::vector<Field> fields;
    BitWriter writer;
    std::size_t expected_bits = 0;
    for (int i = 0; i < 5000; ++i) {
        const int count = static_cast<int>(rng() % 33);
        const auto word = static_cast<std::uint32_t>(rng());
        const std::uint32_t value = count == 0 ? 0 : word >> (32 - count);
        writer.write_bits(value, count);
        fields.push_back({value, count});
        expected_bits += static_cast<std::size_t>(count);
    }
    EXPECT_EQ(writer.bit_count(), expected_bits);

    BitReader reader(writer.bytes().data(), writer.bytes().size());
    for (const Field& field : fields) {
        ASSERT_EQ(reader.read_bits(field.count), field.value);
    }
    EXPECT_LT(reader.bits_remaining(), 8u);
}
