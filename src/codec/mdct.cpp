#include "codec/mdct.hpp"

#include <cmath>
#include <stdexcept>

namespace codec {
namespace {

std::size_t validated_count(std::size_t count) {
    if (count < 2 || (count & (count - 1)) != 0) {
        throw std::invalid_argument("the MDCT size must be a power of two and at least 2");
    }
    return count;
}

}  // namespace

Mdct::Mdct(std::size_t coefficient_count)
    : coefficient_count_(validated_count(coefficient_count)),
      scale_(std::sqrt(2 / static_cast<Sample>(coefficient_count_))),
      window_(2 * coefficient_count_),
      twiddles_(coefficient_count_ / 2),
      fft_(coefficient_count_ / 2) {
    const auto m = static_cast<Sample>(coefficient_count_);
    for (std::size_t n = 0; n < window_.size(); ++n) {
        window_[n] = std::sin(kPi * (static_cast<Sample>(n) + 0.5) / (2 * m));
    }
    for (std::size_t n = 0; n < twiddles_.size(); ++n) {
        const Sample angle = kPi * (static_cast<Sample>(n) + 0.125) / m;
        twiddles_[n] = Fft::Complex(std::cos(angle), -std::sin(angle));
    }
}

std::vector<Sample> Mdct::dct4(const std::vector<Sample>& input) const {
    const std::size_t m = coefficient_count_;

    // Packs the even-indexed values and the reversed odd-indexed values into one complex sequence.
    // The sum over n then equals a complex FFT of M / 2 points between two twiddle rotations.
    std::vector<Fft::Complex> buffer(m / 2);
    for (std::size_t n = 0; n < m / 2; ++n) {
        buffer[n] =
            complex_multiply(Fft::Complex(input[2 * n], input[m - 1 - 2 * n]), twiddles_[n]);
    }
    fft_.transform(buffer);

    std::vector<Sample> output(m);
    for (std::size_t k = 0; k < m / 2; ++k) {
        const Fft::Complex rotated = complex_multiply(buffer[k], twiddles_[k]);
        output[2 * k] = rotated.real();
        output[m - 1 - 2 * k] = -rotated.imag();
    }
    return output;
}

std::vector<Sample> Mdct::forward(const std::vector<Sample>& block) const {
    if (block.size() != block_size()) {
        throw std::invalid_argument("the MDCT input must have 2M samples");
    }
    const std::size_t m = coefficient_count_;
    const std::size_t half = m / 2;

    std::vector<Sample> windowed(block.size());
    for (std::size_t n = 0; n < block.size(); ++n) {
        windowed[n] = window_[n] * block[n];
    }

    // With the block split into four quarters a, b, c and d, the MDCT equals the DCT-IV of
    // (-c_reversed - d, a - b_reversed).
    std::vector<Sample> folded(m);
    for (std::size_t n = 0; n < half; ++n) {
        folded[n] = -windowed[3 * half - 1 - n] - windowed[3 * half + n];
        folded[half + n] = windowed[n] - windowed[m - 1 - n];
    }

    std::vector<Sample> coefficients = dct4(folded);
    for (Sample& value : coefficients) {
        value *= scale_;
    }
    return coefficients;
}

std::vector<Sample> Mdct::inverse(const std::vector<Sample>& coefficients) const {
    if (coefficients.size() != coefficient_count_) {
        throw std::invalid_argument("the IMDCT input must have M coefficients");
    }
    const std::size_t m = coefficient_count_;
    const std::size_t half = m / 2;

    // Writing the DCT-IV output as two halves (v1, v2), the block is (v2, -v2_reversed,
    // -v1_reversed, -v1).
    const std::vector<Sample> folded = dct4(coefficients);
    std::vector<Sample> block(2 * m);
    for (std::size_t n = 0; n < half; ++n) {
        block[n] = folded[half + n];
        block[half + n] = -folded[m - 1 - n];
        block[m + n] = -folded[half - 1 - n];
        block[m + half + n] = -folded[n];
    }
    for (std::size_t n = 0; n < block.size(); ++n) {
        block[n] *= scale_ * window_[n];
    }
    return block;
}

}  // namespace codec
