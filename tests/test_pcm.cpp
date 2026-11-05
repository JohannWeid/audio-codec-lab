#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <vector>

#include "codec/pcm.hpp"
#include "codec/types.hpp"

namespace {

using codec::pcm_to_samples;
using codec::Sample;
using codec::samples_to_pcm;

std::int16_t convert(Sample sample) { return samples_to_pcm({sample})[0]; }

}  // namespace

TEST(Pcm, ScalesByPowerOfTwo) {
    const std::vector<Sample> samples = pcm_to_samples({-32768, -16384, 0, 16384, 32767});
    EXPECT_EQ(samples[0], -1.0);
    EXPECT_EQ(samples[1], -0.5);
    EXPECT_EQ(samples[2], 0.0);
    EXPECT_EQ(samples[3], 0.5);
    EXPECT_EQ(samples[4], 32767.0 / 32768.0);
}

TEST(Pcm, EveryValueRoundtripsExactly) {
    std::vector<std::int16_t> pcm;
    pcm.reserve(65536);
    for (int value = -32768; value <= 32767; ++value) {
        pcm.push_back(static_cast<std::int16_t>(value));
    }
    EXPECT_EQ(samples_to_pcm(pcm_to_samples(pcm)), pcm);
}

TEST(Pcm, RoundsToNearestWithTiesAwayFromZero) {
    const Sample lsb = 1.0 / 32768.0;
    EXPECT_EQ(convert(0.4 * lsb), 0);
    EXPECT_EQ(convert(0.5 * lsb), 1);
    EXPECT_EQ(convert(0.6 * lsb), 1);
    EXPECT_EQ(convert(-0.4 * lsb), 0);
    EXPECT_EQ(convert(-0.5 * lsb), -1);
    EXPECT_EQ(convert(-0.6 * lsb), -1);
    EXPECT_EQ(convert(1.5 * lsb), 2);
    EXPECT_EQ(convert(2.5 * lsb), 3);
}

TEST(Pcm, ClipsValuesOutOfRange) {
    EXPECT_EQ(convert(1.0), 32767);
    EXPECT_EQ(convert(1.5), 32767);
    EXPECT_EQ(convert(-1.0), -32768);
    EXPECT_EQ(convert(-1.5), -32768);
    EXPECT_EQ(convert(std::numeric_limits<Sample>::infinity()), 32767);
    EXPECT_EQ(convert(-std::numeric_limits<Sample>::infinity()), -32768);
    EXPECT_EQ(convert(std::numeric_limits<Sample>::max()), 32767);
}

TEST(Pcm, NanBecomesZero) { EXPECT_EQ(convert(std::numeric_limits<Sample>::quiet_NaN()), 0); }
