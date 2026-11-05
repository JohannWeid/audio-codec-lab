#pragma once

#include <complex>
#include <cstddef>
#include <vector>

#include "codec/types.hpp"

namespace codec {

/// Forward complex radix-2 FFT without normalization:
/// X[k] = sum_{n=0}^{N-1} x[n] * exp(-2 * pi * i * n * k / N)
class Fft {
public:
    using Complex = std::complex<Sample>;

    /// The size must be a power of two, otherwise this throws std::invalid_argument.
    explicit Fft(std::size_t size);

    std::size_t size() const noexcept { return size_; }

    /// Transforms in place. Throws std::invalid_argument if data does not have the planned size.
    void transform(std::vector<Complex>& data) const;

private:
    std::size_t size_;
    std::vector<std::size_t> bit_reversed_;
    std::vector<Complex> twiddles_;
};

/// Written out because std::complex multiplication may take a slower path for special values, and
/// this keeps the operation order the same on every platform.
inline Fft::Complex complex_multiply(const Fft::Complex& a, const Fft::Complex& b) {
    return {a.real() * b.real() - a.imag() * b.imag(), a.real() * b.imag() + a.imag() * b.real()};
}

}  // namespace codec
