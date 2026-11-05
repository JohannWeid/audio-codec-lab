#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "codec/types.hpp"

namespace test {

using codec::kPi;
using codec::Sample;

/// Uniform noise in [-1, 1). The same seed gives the same signal on every platform, which the
/// standard distributions do not guarantee.
inline std::vector<Sample> random_signal(std::size_t size, std::uint32_t seed) {
    std::mt19937 rng(seed);
    std::vector<Sample> signal(size);
    for (Sample& value : signal) {
        value = static_cast<Sample>(rng()) / 4294967296.0 * 2 - 1;
    }
    return signal;
}

inline Sample energy(const std::vector<Sample>& signal) {
    Sample sum = 0;
    for (const Sample value : signal) {
        sum += value * value;
    }
    return sum;
}

inline Sample max_abs_difference(const std::vector<Sample>& a, const std::vector<Sample>& b) {
    Sample largest = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        largest = std::fmax(largest, std::fabs(a[i] - b[i]));
    }
    return largest;
}

inline Sample reference_window(std::size_t n, std::size_t m) {
    return std::sin(kPi * (static_cast<Sample>(n) + 0.5) / (2 * static_cast<Sample>(m)));
}

inline Sample reference_cosine(std::size_t n, std::size_t k, std::size_t m) {
    const auto size = static_cast<Sample>(m);
    return std::cos(kPi / size * (static_cast<Sample>(n) + 0.5 + size / 2) *
                    (static_cast<Sample>(k) + 0.5));
}

/// Slow reference MDCT, written directly from the formula in spec/codec-spec.md.
inline std::vector<Sample> reference_mdct(const std::vector<Sample>& block, std::size_t m) {
    std::vector<Sample> coefficients(m);
    for (std::size_t k = 0; k < m; ++k) {
        Sample sum = 0;
        for (std::size_t n = 0; n < 2 * m; ++n) {
            sum += reference_window(n, m) * block[n] * reference_cosine(n, k, m);
        }
        coefficients[k] = std::sqrt(2 / static_cast<Sample>(m)) * sum;
    }
    return coefficients;
}

/// Slow reference inverse MDCT, written directly from the formula in spec/codec-spec.md.
inline std::vector<Sample> reference_imdct(const std::vector<Sample>& coefficients, std::size_t m) {
    std::vector<Sample> block(2 * m);
    for (std::size_t n = 0; n < 2 * m; ++n) {
        Sample sum = 0;
        for (std::size_t k = 0; k < m; ++k) {
            sum += coefficients[k] * reference_cosine(n, k, m);
        }
        block[n] = std::sqrt(2 / static_cast<Sample>(m)) * reference_window(n, m) * sum;
    }
    return block;
}

}  // namespace test
