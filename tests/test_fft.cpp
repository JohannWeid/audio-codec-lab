#include <gtest/gtest.h>

#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "codec/fft.hpp"
#include "test_helpers.hpp"

namespace {

using codec::Fft;
using codec::kPi;
using codec::Sample;
using Complex = Fft::Complex;

std::vector<Complex> random_complex(std::size_t size, std::uint32_t seed) {
    const std::vector<Sample> noise = test::random_signal(2 * size, seed);
    std::vector<Complex> data(size);
    for (std::size_t i = 0; i < size; ++i) {
        data[i] = Complex(noise[2 * i], noise[2 * i + 1]);
    }
    return data;
}

std::vector<Complex> naive_dft(const std::vector<Complex>& input) {
    const auto size = static_cast<Sample>(input.size());
    std::vector<Complex> output(input.size());
    for (std::size_t k = 0; k < input.size(); ++k) {
        for (std::size_t n = 0; n < input.size(); ++n) {
            const Sample angle = -2 * kPi * static_cast<Sample>(n * k) / size;
            output[k] += input[n] * Complex(std::cos(angle), std::sin(angle));
        }
    }
    return output;
}

}  // namespace

TEST(Fft, MatchesNaiveDft) {
    for (const std::size_t size : {1u, 2u, 4u, 8u, 16u, 64u, 256u, 1024u}) {
        SCOPED_TRACE(size);
        const std::vector<Complex> input = random_complex(size, 1);
        const std::vector<Complex> expected = naive_dft(input);

        std::vector<Complex> actual = input;
        Fft(size).transform(actual);

        for (std::size_t k = 0; k < size; ++k) {
            EXPECT_NEAR(actual[k].real(), expected[k].real(), 1e-10);
            EXPECT_NEAR(actual[k].imag(), expected[k].imag(), 1e-10);
        }
    }
}

TEST(Fft, ImpulseGivesFlatSpectrum) {
    std::vector<Complex> data(16);
    data[0] = Complex(1, 0);
    Fft(16).transform(data);
    for (const Complex& value : data) {
        EXPECT_NEAR(value.real(), 1, 1e-14);
        EXPECT_NEAR(value.imag(), 0, 1e-14);
    }
}

TEST(Fft, ToneFallsIntoOneBin) {
    constexpr std::size_t kSize = 64;
    constexpr std::size_t kBin = 5;
    std::vector<Complex> data(kSize);
    for (std::size_t n = 0; n < kSize; ++n) {
        const Sample angle = 2 * kPi * static_cast<Sample>(kBin * n) / static_cast<Sample>(kSize);
        data[n] = Complex(std::cos(angle), std::sin(angle));
    }
    Fft(kSize).transform(data);
    for (std::size_t k = 0; k < kSize; ++k) {
        EXPECT_NEAR(std::abs(data[k]), k == kBin ? static_cast<Sample>(kSize) : 0.0, 1e-12);
    }
}

TEST(Fft, RejectsSizesThatAreNotPowersOfTwo) {
    for (const std::size_t size : {0u, 3u, 12u, 1000u}) {
        SCOPED_TRACE(size);
        EXPECT_THROW(Fft{size}, std::invalid_argument);
    }
}

TEST(Fft, RejectsDataOfWrongSize) {
    std::vector<Complex> data(8);
    EXPECT_THROW(Fft(16).transform(data), std::invalid_argument);
}
