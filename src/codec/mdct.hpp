#pragma once

#include <cstddef>
#include <vector>

#include "codec/fft.hpp"
#include "codec/types.hpp"

namespace codec {

/// Fast orthonormal MDCT with a sine window, as defined in section 2 of spec/codec-spec.md. One
/// block of 2M samples gives M coefficients. It uses a complex FFT of M / 2 points.
class Mdct {
public:
    /// M must be a power of two and at least 2, otherwise this throws std::invalid_argument.
    explicit Mdct(std::size_t coefficient_count);

    std::size_t coefficient_count() const noexcept { return coefficient_count_; }
    std::size_t block_size() const noexcept { return 2 * coefficient_count_; }
    const std::vector<Sample>& window() const noexcept { return window_; }

    /// Takes 2M samples, applies the window and returns M coefficients. Throws
    /// std::invalid_argument for any other input size.
    std::vector<Sample> forward(const std::vector<Sample>& block) const;

    /// Takes M coefficients and returns 2M windowed samples that still contain time-domain
    /// aliasing, which overlap-add with the neighboring blocks cancels. Throws
    /// std::invalid_argument for any other input size.
    std::vector<Sample> inverse(const std::vector<Sample>& coefficients) const;

private:
    // Unnormalized DCT-IV of M values: X[k] = sum_n u[n] * cos(pi / M * (n + 1/2) * (k + 1/2))
    std::vector<Sample> dct4(const std::vector<Sample>& input) const;

    std::size_t coefficient_count_;
    Sample scale_;
    std::vector<Sample> window_;
    std::vector<Fft::Complex> twiddles_;
    Fft fft_;
};

}  // namespace codec
