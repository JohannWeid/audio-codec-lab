#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

#include "codec/entropy.hpp"

namespace {

using Bytes = std::vector<std::uint8_t>;
using codec::AdaptiveValue;
using codec::bit_length;
using codec::decode_value;
using codec::encode_value;
using codec::RangeDecoder;
using codec::RangeEncoder;
using codec::zigzag_decode;
using codec::zigzag_encode;

constexpr std::uint32_t kMax32 = std::numeric_limits<std::uint32_t>::max();

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

TEST(BitLength, MatchesKnownValues) {
    EXPECT_EQ(bit_length(0), 0);
    EXPECT_EQ(bit_length(1), 1);
    EXPECT_EQ(bit_length(2), 2);
    EXPECT_EQ(bit_length(3), 2);
    EXPECT_EQ(bit_length(4), 3);
    EXPECT_EQ(bit_length(255), 8);
    EXPECT_EQ(bit_length(256), 9);
    EXPECT_EQ(bit_length(kMax32), 32);
}

TEST(AdaptiveValue, RoundtripsValuesOfEverySize) {
    const std::uint32_t values[] = {0,        1,          2,     3,        4,        5,
                                    7,        8,          15,    16,       255,      256,
                                    1000,     65535,      65536, 1u << 24, 1u << 30, (1u << 30) + 1,
                                    1u << 31, kMax32 - 1, kMax32};
    RangeEncoder encoder;
    AdaptiveValue encoder_model;
    for (const std::uint32_t value : values) {
        encode_value(encoder, encoder_model, value);
    }
    const Bytes bytes = encoder.finish();

    RangeDecoder decoder(bytes.data(), bytes.size());
    AdaptiveValue decoder_model;
    for (const std::uint32_t value : values) {
        EXPECT_EQ(decode_value(decoder, decoder_model), value);
    }
    EXPECT_TRUE(decoder.has_read_all_data());
}

TEST(AdaptiveValue, RoundtripsARandomMixOfSizes) {
    std::mt19937 rng(81);
    std::vector<std::uint32_t> values;
    RangeEncoder encoder;
    AdaptiveValue encoder_model;
    for (int i = 0; i < 50000; ++i) {
        const auto value = static_cast<std::uint32_t>(rng()) >> (rng() % 32);
        encode_value(encoder, encoder_model, value);
        values.push_back(value);
    }
    const Bytes bytes = encoder.finish();

    RangeDecoder decoder(bytes.data(), bytes.size());
    AdaptiveValue decoder_model;
    for (const std::uint32_t value : values) {
        ASSERT_EQ(decode_value(decoder, decoder_model), value);
    }
}

// This is what a Rice code cannot do: it needs at least one bit for every value, even a zero.
TEST(AdaptiveValue, SpendsFarLessThanOneBitPerValueOnSparseData) {
    std::mt19937 rng(82);
    const std::size_t count = 100000;
    RangeEncoder encoder;
    AdaptiveValue model;
    for (std::size_t i = 0; i < count; ++i) {
        encode_value(encoder, model, rng() % 50 == 0 ? 1u : 0u);
    }
    const double bits_per_value =
        8.0 * static_cast<double>(encoder.finish().size()) / static_cast<double>(count);
    EXPECT_LT(bits_per_value, 0.2);
}

// The sanitizer job in CI turns any undefined behavior here into a failure.
TEST(AdaptiveValue, RandomDataIsHandled) {
    std::mt19937 rng(83);
    Bytes bytes(4096);
    for (int round = 0; round < 50; ++round) {
        for (std::uint8_t& byte : bytes) {
            byte = static_cast<std::uint8_t>(rng() & 0xFFu);
        }
        RangeDecoder decoder(bytes.data(), bytes.size());
        AdaptiveValue model;
        for (int i = 0; i < 5000; ++i) {
            (void)decode_value(decoder, model);
        }
    }
}
