#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <vector>

#include "codec/mdct.hpp"
#include "test_helpers.hpp"

namespace {

using codec::kPi;
using codec::Mdct;
using codec::Sample;

constexpr std::size_t kSizes[] = {2, 4, 8, 16, 64, 256, 1024};

}  // namespace

// Checks the reference itself against values worked out by hand, so that the fast MDCT is not only
// compared with code of the same origin. For M = 2 and an impulse at n = 0, the formula reduces to
// X[0] = sin(pi/8)^2 and X[1] = -sin(pi/8) * cos(pi/8).
TEST(MdctReference, MatchesHandComputedValues) {
    const std::vector<Sample> coefficients = test::reference_mdct({1, 0, 0, 0}, 2);
    EXPECT_NEAR(coefficients[0], std::sin(kPi / 8) * std::sin(kPi / 8), 1e-15);
    EXPECT_NEAR(coefficients[1], -std::sin(kPi / 8) * std::cos(kPi / 8), 1e-15);
}

TEST(Mdct, WindowMatchesSpecification) {
    for (const std::size_t m : kSizes) {
        SCOPED_TRACE(m);
        const Mdct mdct(m);
        ASSERT_EQ(mdct.window().size(), 2 * m);
        for (std::size_t n = 0; n < 2 * m; ++n) {
            EXPECT_NEAR(mdct.window()[n], test::reference_window(n, m), 1e-15);
        }
    }
}

TEST(Mdct, WindowSatisfiesPrincenBradley) {
    for (const std::size_t m : kSizes) {
        SCOPED_TRACE(m);
        const Mdct mdct(m);
        const std::vector<Sample>& w = mdct.window();
        for (std::size_t n = 0; n < m; ++n) {
            EXPECT_NEAR(w[n] * w[n] + w[n + m] * w[n + m], 1, 1e-15);
        }
    }
}

TEST(Mdct, ForwardMatchesReference) {
    for (const std::size_t m : kSizes) {
        SCOPED_TRACE(m);
        const std::vector<Sample> block = test::random_signal(2 * m, 2);
        const std::vector<Sample> fast = Mdct(m).forward(block);
        const std::vector<Sample> slow = test::reference_mdct(block, m);
        ASSERT_EQ(fast.size(), m);
        EXPECT_LT(test::max_abs_difference(fast, slow), 1e-11);
    }
}

TEST(Mdct, InverseMatchesReference) {
    for (const std::size_t m : kSizes) {
        SCOPED_TRACE(m);
        const std::vector<Sample> coefficients = test::random_signal(m, 3);
        const std::vector<Sample> fast = Mdct(m).inverse(coefficients);
        const std::vector<Sample> slow = test::reference_imdct(coefficients, m);
        ASSERT_EQ(fast.size(), 2 * m);
        EXPECT_LT(test::max_abs_difference(fast, slow), 1e-11);
    }
}

TEST(Mdct, RejectsInvalidSizes) {
    for (const std::size_t m : {0u, 1u, 3u, 6u, 1000u}) {
        SCOPED_TRACE(m);
        EXPECT_THROW(Mdct{m}, std::invalid_argument);
    }
}

TEST(Mdct, RejectsInputOfWrongSize) {
    const Mdct mdct(8);
    EXPECT_THROW(mdct.forward(std::vector<Sample>(8)), std::invalid_argument);
    EXPECT_THROW(mdct.inverse(std::vector<Sample>(16)), std::invalid_argument);
}
