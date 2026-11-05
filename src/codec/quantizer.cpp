#include "codec/quantizer.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <stdexcept>

namespace codec {
namespace {

constexpr int kStepsPerOctave = 8;
constexpr int kStepOffset = 144;

// 2^(j / 8) for j = 0 to 7.
constexpr std::array<Sample, kStepsPerOctave> kOctaveFractions = {
    1.0,
    1.0905077326652577,
    1.189207115002721,
    1.2968395546510096,
    1.4142135623730951,
    1.5422108254079408,
    1.681792830507429,
    1.8340080864093424,
};

}  // namespace

Sample step_size(int gain, int scalefactor) {
    if (gain < kMinGain || gain > kMaxGain) {
        throw std::invalid_argument("the gain must be in 0 to 255");
    }
    if (scalefactor < kMinScalefactor || scalefactor > kMaxScalefactor) {
        throw std::invalid_argument("the scalefactor must be in -128 to 127");
    }
    // The offset makes the index positive before splitting it, so that the division rounds down.
    const int shifted = gain - scalefactor - kStepOffset + 64 * kStepsPerOctave;
    const int octave = shifted / kStepsPerOctave - 64;
    const auto fraction = static_cast<std::size_t>(shifted % kStepsPerOctave);
    return std::ldexp(kOctaveFractions[fraction], octave);
}

std::int32_t quantize(Sample value, Sample step) noexcept {
    const Sample magnitude = std::floor(std::fabs(value) / step + 0.5);
    if (std::isnan(magnitude)) {
        return 0;
    }
    const std::int32_t limited = magnitude >= static_cast<Sample>(kMaxQuantized)
                                     ? kMaxQuantized
                                     : static_cast<std::int32_t>(magnitude);
    return value < 0 ? -limited : limited;
}

Sample dequantize(std::int32_t quantized, Sample step) noexcept {
    return static_cast<Sample>(quantized) * step;
}

}  // namespace codec
