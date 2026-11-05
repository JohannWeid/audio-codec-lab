#include "codec/fft.hpp"

#include <cmath>
#include <stdexcept>
#include <utility>

namespace codec {
namespace {

std::size_t validated_size(std::size_t size) {
    if (size == 0 || (size & (size - 1)) != 0) {
        throw std::invalid_argument("the FFT size must be a power of two");
    }
    return size;
}

}  // namespace

Fft::Fft(std::size_t size)
    : size_(validated_size(size)), bit_reversed_(size_), twiddles_(size_ / 2) {
    for (std::size_t i = 1; i < size_; ++i) {
        bit_reversed_[i] = bit_reversed_[i / 2] / 2;
        if (i % 2 != 0) {
            bit_reversed_[i] += size_ / 2;
        }
    }
    for (std::size_t k = 0; k < twiddles_.size(); ++k) {
        const Sample angle = 2 * kPi * static_cast<Sample>(k) / static_cast<Sample>(size_);
        twiddles_[k] = Complex(std::cos(angle), -std::sin(angle));
    }
}

void Fft::transform(std::vector<Complex>& data) const {
    if (data.size() != size_) {
        throw std::invalid_argument("the data does not have the planned FFT size");
    }
    for (std::size_t i = 0; i < size_; ++i) {
        if (i < bit_reversed_[i]) {
            std::swap(data[i], data[bit_reversed_[i]]);
        }
    }
    for (std::size_t length = 2; length <= size_; length *= 2) {
        const std::size_t half = length / 2;
        const std::size_t step = size_ / length;
        for (std::size_t start = 0; start < size_; start += length) {
            for (std::size_t j = 0; j < half; ++j) {
                const Complex odd = complex_multiply(data[start + j + half], twiddles_[j * step]);
                data[start + j + half] = data[start + j] - odd;
                data[start + j] += odd;
            }
        }
    }
}

}  // namespace codec
