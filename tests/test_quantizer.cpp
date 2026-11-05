#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

#include "codec/bands.hpp"
#include "codec/quantizer.hpp"
#include "codec/types.hpp"
#include "test_helpers.hpp"

namespace {

using codec::dequantize;
using codec::kBandCount;
using codec::kBandEdges;
using codec::kMaxGain;
using codec::kMaxQuantized;
using codec::kMaxScalefactor;
using codec::kMinGain;
using codec::kMinScalefactor;
using codec::quantize;
using codec::Sample;
using codec::step_size;

Sample bark(Sample frequency) {
    return 13 * std::atan(0.00076 * frequency) + 3.5 * std::atan(std::pow(frequency / 7500, 2));
}

}  // namespace

TEST(Bands, CoverAllCoefficientsWithoutGaps) {
    EXPECT_EQ(kBandEdges.size(), kBandCount + 1);
    EXPECT_EQ(kBandEdges.front(), 0u);
    EXPECT_EQ(kBandEdges.back(), 1024u);
    for (std::size_t b = 0; b < kBandCount; ++b) {
        EXPECT_GE(kBandEdges[b + 1] - kBandEdges[b], 4u);
    }
}

// A band starts where the Bark value first reaches the next integer, unless the minimum width of 4
// coefficients forces it to start later.
TEST(Bands, FollowTheBarkScale) {
    for (std::size_t b = 1; b < kBandCount; ++b) {
        SCOPED_TRACE(b);
        const std::size_t edge = kBandEdges[b];
        const Sample at_edge = bark((static_cast<Sample>(edge) + 0.5) * 23.4375);
        const Sample before_edge = bark((static_cast<Sample>(edge) - 0.5) * 23.4375);
        EXPECT_GT(std::floor(at_edge),
                  std::floor(bark((static_cast<Sample>(kBandEdges[b - 1]) + 0.5) * 23.4375)));
        if (kBandEdges[b] - kBandEdges[b - 1] > 4) {
            EXPECT_LT(std::floor(before_edge), std::floor(at_edge));
        }
    }
}

TEST(StepSize, MatchesFormula) {
    for (int gain = kMinGain; gain <= kMaxGain; ++gain) {
        for (const int scalefactor : {kMinScalefactor, -1, 0, 1, kMaxScalefactor}) {
            SCOPED_TRACE(testing::Message() << "gain=" << gain << " scalefactor=" << scalefactor);
            const Sample expected = std::pow(2.0, (gain - scalefactor - 144) / 8.0);
            EXPECT_NEAR(step_size(gain, scalefactor) / expected, 1, 1e-14);
        }
    }
}

// These must hold exactly, because the table is meant to give the same double on every platform.
TEST(StepSize, DoublesExactlyEveryEightUnits) {
    EXPECT_EQ(step_size(144, 0), 1.0);
    EXPECT_EQ(step_size(152, 0), 2.0);
    EXPECT_EQ(step_size(136, 0), 0.5);
    EXPECT_EQ(step_size(0, 0), std::ldexp(1.0, -18));
    for (int gain = kMinGain; gain + 8 <= kMaxGain; ++gain) {
        EXPECT_EQ(step_size(gain + 8, 0), 2 * step_size(gain, 0));
    }
}

TEST(StepSize, GainAndScalefactorPullInOppositeDirections) {
    EXPECT_EQ(step_size(100, 8), step_size(92, 0));
    EXPECT_EQ(step_size(100, -8), step_size(108, 0));
    for (int gain = kMinGain; gain < kMaxGain; ++gain) {
        EXPECT_GT(step_size(gain + 1, 0), step_size(gain, 0));
    }
    EXPECT_GT(step_size(100, 0), step_size(100, 1));
}

TEST(StepSize, RejectsValuesOutOfRange) {
    EXPECT_THROW(step_size(kMinGain - 1, 0), std::invalid_argument);
    EXPECT_THROW(step_size(kMaxGain + 1, 0), std::invalid_argument);
    EXPECT_THROW(step_size(100, kMinScalefactor - 1), std::invalid_argument);
    EXPECT_THROW(step_size(100, kMaxScalefactor + 1), std::invalid_argument);
}

TEST(Quantize, RoundsToNearestWithTiesAwayFromZero) {
    EXPECT_EQ(quantize(0.0, 1.0), 0);
    EXPECT_EQ(quantize(0.49, 1.0), 0);
    EXPECT_EQ(quantize(0.5, 1.0), 1);
    EXPECT_EQ(quantize(1.49, 1.0), 1);
    EXPECT_EQ(quantize(-0.49, 1.0), 0);
    EXPECT_EQ(quantize(-0.5, 1.0), -1);
    EXPECT_EQ(quantize(-1.49, 1.0), -1);
    EXPECT_EQ(quantize(10.0, 4.0), 3);
    EXPECT_EQ(quantize(-10.0, 4.0), -3);
}

TEST(Quantize, ErrorStaysWithinHalfAStep) {
    const auto values = test::random_signal(5000, 24);
    for (const int gain : {80, 120, 144, 160}) {
        const Sample step = step_size(gain, 0);
        for (const Sample value : values) {
            const Sample scaled = value * 30;
            const Sample error = scaled - dequantize(quantize(scaled, step), step);
            ASSERT_LE(std::fabs(error), step / 2 * (1 + 1e-12));
        }
    }
}

TEST(Quantize, CoarseStepGivesAllZeros) {
    const Sample coarsest = step_size(kMaxGain, 0);
    // The largest coefficient of a full-scale signal stays below sqrt(2M), which is about 45.
    EXPECT_EQ(quantize(45.0, coarsest), 0);
    EXPECT_EQ(quantize(-45.0, coarsest), 0);
}

TEST(Quantize, FinestStepStaysFarBelowTheLimit) {
    const std::int32_t largest = quantize(45.0, step_size(kMinGain, 0));
    EXPECT_GT(largest, 1 << 23);
    EXPECT_LT(largest, kMaxQuantized);
}

TEST(Quantize, LimitsAndCleansSpecialValues) {
    const Sample huge = std::numeric_limits<Sample>::max();
    const Sample infinity = std::numeric_limits<Sample>::infinity();
    EXPECT_EQ(quantize(huge, 1.0), kMaxQuantized);
    EXPECT_EQ(quantize(-huge, 1.0), -kMaxQuantized);
    EXPECT_EQ(quantize(infinity, 1.0), kMaxQuantized);
    EXPECT_EQ(quantize(-infinity, 1.0), -kMaxQuantized);
    EXPECT_EQ(quantize(std::numeric_limits<Sample>::quiet_NaN(), 1.0), 0);
}

TEST(Dequantize, MultipliesByTheStep) {
    EXPECT_EQ(dequantize(0, 0.25), 0.0);
    EXPECT_EQ(dequantize(3, 0.25), 0.75);
    EXPECT_EQ(dequantize(-3, 0.25), -0.75);
}
