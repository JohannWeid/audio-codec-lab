#include <gtest/gtest.h>

#include <cmath>

// Guards the determinism rules in spec/codec-spec.md. If a compiler turns a * b + c into one fused
// multiply-add instruction, the result differs from a platform that rounds after the
// multiplication. This test fails on such a platform if the flags in CMakeLists.txt are missing.
TEST(FloatingPoint, MultiplyAddIsNotFused) {
    // volatile keeps the compiler from computing the result at compile time.
    volatile double factor_input = 1.0 + std::ldexp(1.0, -30);
    volatile double offset_input = -(1.0 + std::ldexp(1.0, -29));
    const double factor = factor_input;
    const double offset = offset_input;

    // factor * factor is exactly 1 + 2^-29 + 2^-60. Rounded to double, the last term is lost and
    // adding the offset gives exactly 0. A fused operation keeps it and gives 2^-60 instead.
    const double result = factor * factor + offset;

    EXPECT_EQ(result, 0.0);
}
