#include "codec/allocation.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

#include "codec/quantizer.hpp"

namespace codec {
namespace {

constexpr int kStepsPerOctave = 8;

// Tries out one gain on a copy of the state, so that a rejected attempt leaves no trace.
EncodedFrame encode_at_gain(const std::vector<Sample>& coefficients, int gain,
                            const Scalefactors& scalefactors, const FrameCoderState& state) {
    EncodedFrame result;
    result.frame = quantize_frame(coefficients, gain, scalefactors);
    result.state = state;
    result.payload = encode_frame_payload(result.frame, result.state);
    return result;
}

// Sets all coefficients of a band to zero if its whole energy lies below the threshold in quiet, so
// that nobody can hear it in any case. Without this, a large budget goes to content such as the
// rounding noise of the input. Bands that are only masked by the rest of the signal stay, because
// they are part of that signal: their coarse steps already remove them if bits are short.
std::vector<Sample> without_inaudible_bands(const std::vector<Sample>& coefficients,
                                            const BandValues& quiet_thresholds) {
    std::vector<Sample> audible = coefficients;
    for (std::size_t band = 0; band < kBandCount; ++band) {
        Sample energy = 0;
        for (std::size_t k = kBandEdges[band]; k < kBandEdges[band + 1]; ++k) {
            energy += coefficients[k] * coefficients[k];
        }
        if (energy <= quiet_thresholds[band]) {
            std::fill(audible.begin() + static_cast<std::ptrdiff_t>(kBandEdges[band]),
                      audible.begin() + static_cast<std::ptrdiff_t>(kBandEdges[band + 1]),
                      Sample{0});
        }
    }
    return audible;
}

// How many of the lowest bands a budget can afford, with at least the given eighths of a bit for
// each coded coefficient. The bands above them stay empty. Each band counts about the same for the
// ear, but the high ones hold far more coefficients. If bits are very short for all bands, it is
// better to serve the low ones properly than to spread too few bits over everything.
std::size_t affordable_band_count(std::size_t max_payload_bytes,
                                  std::uint32_t bandwidth_limit_eighths) noexcept {
    const std::size_t affordable_coefficients = max_payload_bytes * 8 * 8 / bandwidth_limit_eighths;
    std::size_t bands = kBandCount;
    while (bands > 1 && kBandEdges[bands] > affordable_coefficients) {
        --bands;
    }
    return bands;
}

}  // namespace

Scalefactors scalefactors_for_thresholds(const BandValues& thresholds) {
    Scalefactors scalefactors{};
    for (std::size_t band = 0; band < kBandCount; ++band) {
        if (thresholds[band] <= 0) {
            scalefactors[band] = kMaxScalefactor;
            continue;
        }
        const auto width = static_cast<Sample>(kBandEdges[band + 1] - kBandEdges[band]);
        const Sample step = std::sqrt(12 * thresholds[band] / width);
        const auto wanted = std::lround(-kStepsPerOctave * std::log2(step));
        scalefactors[band] = static_cast<int>(std::clamp(wanted, static_cast<long>(kMinScalefactor),
                                                         static_cast<long>(kMaxScalefactor)));
    }
    return scalefactors;
}

BandValues band_noise(const std::vector<Sample>& coefficients, const FrameData& frame) {
    const std::vector<Sample> decoded = dequantize_frame(frame);
    BandValues noise{};
    for (std::size_t band = 0; band < kBandCount; ++band) {
        for (std::size_t k = kBandEdges[band]; k < kBandEdges[band + 1]; ++k) {
            const Sample difference = coefficients[k] - decoded[k];
            noise[band] += difference * difference;
        }
    }
    return noise;
}

Sample mean_excess_db(const BandValues& noise, const BandValues& thresholds) {
    Sample sum = 0;
    for (std::size_t band = 0; band < kBandCount; ++band) {
        if (noise[band] > thresholds[band]) {
            sum += 10 * std::log10(noise[band] / thresholds[band]);
        }
    }
    return sum / static_cast<Sample>(kBandCount);
}

EncodedFrame encode_with_scalefactors(const std::vector<Sample>& coefficients,
                                      const Scalefactors& scalefactors,
                                      std::size_t max_payload_bytes, const FrameCoderState& state) {
    EncodedFrame best = encode_at_gain(coefficients, kMaxGain, scalefactors, state);
    if (best.payload.size() > max_payload_bytes) {
        return encode_at_gain(std::vector<Sample>(kFrameLength), kMaxGain, Scalefactors{}, state);
    }

    int fits = kMaxGain;
    int too_large = kMinGain - 1;
    while (fits - too_large > 1) {
        const int gain = too_large + (fits - too_large) / 2;
        EncodedFrame candidate = encode_at_gain(coefficients, gain, scalefactors, state);
        if (candidate.payload.size() <= max_payload_bytes) {
            fits = gain;
            best = std::move(candidate);
        } else {
            too_large = gain;
        }
    }
    return best;
}

EncodedFrame encode_flat(const std::vector<Sample>& coefficients, std::size_t max_payload_bytes,
                         const FrameCoderState& state) {
    return encode_with_scalefactors(coefficients, Scalefactors{}, max_payload_bytes, state);
}

EncodedFrame encode_perceptual(const std::vector<Sample>& coefficients,
                               const MaskingAnalysis& analysis, std::size_t max_payload_bytes,
                               const FrameCoderState& state,
                               std::uint32_t bandwidth_limit_eighths) {
    if (bandwidth_limit_eighths < kMinBandwidthLimitEighths ||
        bandwidth_limit_eighths > kMaxBandwidthLimitEighths) {
        throw std::invalid_argument("the bandwidth limit must be 1 to 64 eighths of a bit");
    }
    std::vector<Sample> audible = without_inaudible_bands(coefficients, analysis.quiet_threshold);
    std::fill(
        audible.begin() +
            static_cast<std::ptrdiff_t>(
                kBandEdges[affordable_band_count(max_payload_bytes, bandwidth_limit_eighths)]),
        audible.end(), Sample{0});
    return encode_with_scalefactors(
        audible, scalefactors_for_thresholds(analysis.masking_threshold), max_payload_bytes, state);
}

}  // namespace codec
