#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "codec/range_coder.hpp"

namespace {

using Bytes = std::vector<std::uint8_t>;
using codec::AdaptiveBit;
using codec::kProbabilityOne;
using codec::RangeDecoder;
using codec::RangeEncoder;

std::vector<bool> random_bits(std::size_t count, std::uint32_t one_in_1000, std::uint32_t seed) {
    std::mt19937 rng(seed);
    std::vector<bool> bits(count);
    for (std::size_t i = 0; i < count; ++i) {
        bits[i] = rng() % 1000 < one_in_1000;
    }
    return bits;
}

double binary_entropy(double probability) {
    return -probability * std::log2(probability) - (1 - probability) * std::log2(1 - probability);
}

}  // namespace

TEST(AdaptiveBit, StartsAtEvenOdds) {
    EXPECT_EQ(AdaptiveBit{}.probability_of_zero(), kProbabilityOne / 2);
}

TEST(AdaptiveBit, LearnsFromDecisions) {
    AdaptiveBit zeros;
    AdaptiveBit ones;
    for (int i = 0; i < 2000; ++i) {
        zeros.update(false);
        ones.update(true);
    }
    EXPECT_GT(zeros.probability_of_zero(), kProbabilityOne * 99 / 100);
    EXPECT_LT(ones.probability_of_zero(), kProbabilityOne / 100);
}

TEST(AdaptiveBit, AlwaysStaysUsableForTheCoder) {
    AdaptiveBit model;
    for (int i = 0; i < 100000; ++i) {
        model.update(false);
    }
    EXPECT_LE(model.probability_of_zero(), kProbabilityOne - 1);
    for (int i = 0; i < 100000; ++i) {
        model.update(true);
    }
    EXPECT_GE(model.probability_of_zero(), 1u);
}

TEST(RangeCoder, RoundtripsFixedProbabilities) {
    for (const std::uint32_t probability : {1u, 100u, 2048u, 4000u, 4095u}) {
        SCOPED_TRACE(probability);
        const std::vector<bool> bits = random_bits(20000, 300, probability);
        RangeEncoder encoder;
        for (const bool bit : bits) {
            encoder.encode(probability, bit);
        }
        const Bytes bytes = encoder.finish();
        RangeDecoder decoder(bytes.data(), bytes.size());
        for (std::size_t i = 0; i < bits.size(); ++i) {
            ASSERT_EQ(decoder.decode(probability), bits[i]) << i;
        }
        EXPECT_TRUE(decoder.has_read_all_data());
    }
}

TEST(RangeCoder, RoundtripsAdaptiveProbabilities) {
    for (const std::uint32_t one_in_1000 : {0u, 1u, 50u, 500u, 950u, 1000u}) {
        SCOPED_TRACE(one_in_1000);
        const std::vector<bool> bits = random_bits(50000, one_in_1000, 71);
        RangeEncoder encoder;
        AdaptiveBit encoder_model;
        for (const bool bit : bits) {
            encoder.encode(encoder_model, bit);
        }
        const Bytes bytes = encoder.finish();
        RangeDecoder decoder(bytes.data(), bytes.size());
        AdaptiveBit decoder_model;
        for (std::size_t i = 0; i < bits.size(); ++i) {
            ASSERT_EQ(decoder.decode(decoder_model), bits[i]) << i;
        }
    }
}

TEST(RangeCoder, RoundtripsDirectBits) {
    std::mt19937 rng(72);
    std::vector<std::uint32_t> values;
    std::vector<int> counts;
    RangeEncoder encoder;
    for (int i = 0; i < 5000; ++i) {
        const int count = static_cast<int>(rng() % 32);
        const auto value = static_cast<std::uint32_t>(rng() & ((1ull << count) - 1));
        encoder.encode_direct(value, count);
        values.push_back(value);
        counts.push_back(count);
    }
    const Bytes bytes = encoder.finish();
    RangeDecoder decoder(bytes.data(), bytes.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
        ASSERT_EQ(decoder.decode_direct(counts[i]), values[i]);
    }
}

// A long run of the most likely decision at the upper end of the range produces bytes of 0xFF,
// which a later carry has to change. This is the classic place for a range coder to go wrong.
TEST(RangeCoder, HandlesCarriesThroughManyBytes) {
    for (std::uint32_t seed = 0; seed < 200; ++seed) {
        SCOPED_TRACE(seed);
        std::mt19937 rng(seed);
        std::vector<bool> bits;
        std::vector<std::uint32_t> probabilities;
        RangeEncoder encoder;
        for (int i = 0; i < 2000; ++i) {
            // Mostly the upper part of a range that is nearly all "one".
            const std::uint32_t probability = rng() % 50 == 0 ? 2048u : 1u;
            const bool bit = rng() % 50 != 0;
            encoder.encode(probability, bit);
            bits.push_back(bit);
            probabilities.push_back(probability);
        }
        const Bytes bytes = encoder.finish();
        RangeDecoder decoder(bytes.data(), bytes.size());
        for (std::size_t i = 0; i < bits.size(); ++i) {
            ASSERT_EQ(decoder.decode(probabilities[i]), bits[i]) << i;
        }
    }
}

TEST(RangeCoder, SpendsFarLessThanOneBitOnLikelyDecisions) {
    const std::size_t count = 200000;
    for (const std::uint32_t one_in_1000 : {10u, 50u, 200u}) {
        SCOPED_TRACE(one_in_1000);
        const std::vector<bool> bits = random_bits(count, one_in_1000, 73);
        RangeEncoder encoder;
        AdaptiveBit model;
        for (const bool bit : bits) {
            encoder.encode(model, bit);
        }
        const double bits_per_decision =
            8.0 * static_cast<double>(encoder.finish().size()) / static_cast<double>(count);
        const double entropy = binary_entropy(one_in_1000 / 1000.0);
        EXPECT_GT(bits_per_decision, 0.95 * entropy);
        EXPECT_LT(bits_per_decision, 1.25 * entropy);
    }
}

TEST(RangeCoder, CostOfAFixedProbabilityMatchesTheory) {
    // 4096 decisions of a probability of 1/2 take 4096 bits, that is 512 bytes.
    RangeEncoder encoder;
    for (int i = 0; i < 4096; ++i) {
        encoder.encode(kProbabilityOne / 2, i % 3 == 0);
    }
    EXPECT_NEAR(static_cast<double>(encoder.finish().size()), 512.0, 2.0);
}

TEST(RangeCoder, NoDecisionsGiveNoBytes) { EXPECT_TRUE(RangeEncoder{}.finish().empty()); }

TEST(RangeCoder, NeverEndsWithAZeroByte) {
    std::mt19937 rng(74);
    for (int round = 0; round < 2000; ++round) {
        RangeEncoder encoder;
        const std::uint32_t count = rng() % 64;
        for (std::uint32_t i = 0; i < count; ++i) {
            encoder.encode(1 + static_cast<std::uint32_t>(rng() % 4095), rng() % 2 == 0);
        }
        const Bytes bytes = encoder.finish();
        if (!bytes.empty()) {
            ASSERT_NE(bytes.back(), 0);
        }
    }
}

TEST(RangeDecoder, ReadsZerosPastTheEndWithoutProblems) {
    RangeDecoder decoder(nullptr, 0);
    for (int i = 0; i < 1000; ++i) {
        EXPECT_FALSE(decoder.decode(kProbabilityOne / 2));
    }
    EXPECT_EQ(decoder.decode_direct(16), 0u);
    EXPECT_TRUE(decoder.has_read_all_data());
}

TEST(RangeDecoder, NoticesDataThatWasNotRead) {
    const Bytes bytes(100, 0xAB);
    RangeDecoder decoder(bytes.data(), bytes.size());
    EXPECT_FALSE(decoder.has_read_all_data());
    for (int i = 0; i < 2000; ++i) {
        (void)decoder.decode(kProbabilityOne / 2);
    }
    EXPECT_TRUE(decoder.has_read_all_data());
}

// The sanitizer job in CI turns any read out of bounds or undefined behavior into a failure.
TEST(RangeDecoder, RandomDataIsHandled) {
    std::mt19937 rng(75);
    Bytes bytes(64);
    for (int round = 0; round < 500; ++round) {
        for (std::uint8_t& byte : bytes) {
            byte = static_cast<std::uint8_t>(rng() & 0xFFu);
        }
        RangeDecoder decoder(bytes.data(), bytes.size());
        AdaptiveBit model;
        for (int i = 0; i < 1000; ++i) {
            (void)decoder.decode(model);
            (void)decoder.decode_direct(3);
        }
    }
}
