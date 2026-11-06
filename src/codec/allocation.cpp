#include "codec/allocation.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include "codec/quantizer.hpp"

namespace codec {
namespace {

// The outer loop refines a band by 1.5 dB per round.
constexpr int kOuterLoopStep = 2;
constexpr int kOuterLoopRounds = 8;

constexpr int kStepsPerOctave = 8;

EncodedFrame encode_at_gain(const std::vector<Sample>& coefficients, int gain,
                            const Scalefactors& scalefactors) {
    EncodedFrame result;
    result.frame = quantize_frame(coefficients, gain, scalefactors);
    BitWriter writer;
    write_frame_payload(result.frame, writer);
    result.payload = writer.bytes();
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
                                      std::size_t max_payload_bytes) {
    EncodedFrame best = encode_at_gain(coefficients, kMaxGain, scalefactors);
    if (best.payload.size() > max_payload_bytes) {
        return encode_at_gain(std::vector<Sample>(kFrameLength), kMaxGain, Scalefactors{});
    }

    int fits = kMaxGain;
    int too_large = kMinGain - 1;
    while (fits - too_large > 1) {
        const int gain = too_large + (fits - too_large) / 2;
        EncodedFrame candidate = encode_at_gain(coefficients, gain, scalefactors);
        if (candidate.payload.size() <= max_payload_bytes) {
            fits = gain;
            best = std::move(candidate);
        } else {
            too_large = gain;
        }
    }
    return best;
}

EncodedFrame encode_flat(const std::vector<Sample>& coefficients, std::size_t max_payload_bytes) {
    return encode_with_scalefactors(coefficients, Scalefactors{}, max_payload_bytes);
}

EncodedFrame encode_perceptual(const std::vector<Sample>& coefficients,
                               const MaskingAnalysis& analysis, std::size_t max_payload_bytes) {
    const BandValues& thresholds = analysis.masking_threshold;
    const std::vector<Sample> audible =
        without_inaudible_bands(coefficients, analysis.quiet_threshold);
    Scalefactors scalefactors = scalefactors_for_thresholds(thresholds);
    EncodedFrame best = encode_with_scalefactors(audible, scalefactors, max_payload_bytes);
    Sample best_excess = mean_excess_db(band_noise(coefficients, best.frame), thresholds);

    for (int round = 0; round < kOuterLoopRounds && best_excess > 0; ++round) {
        const BandValues noise = band_noise(coefficients, best.frame);
        bool changed = false;
        for (std::size_t band = 0; band < kBandCount; ++band) {
            if (noise[band] > thresholds[band] && scalefactors[band] < kMaxScalefactor) {
                scalefactors[band] = std::min(scalefactors[band] + kOuterLoopStep, kMaxScalefactor);
                changed = true;
            }
        }
        if (!changed) {
            break;
        }
        EncodedFrame candidate = encode_with_scalefactors(audible, scalefactors, max_payload_bytes);
        const Sample excess = mean_excess_db(band_noise(coefficients, candidate.frame), thresholds);
        if (excess >= best_excess) {
            break;
        }
        best = std::move(candidate);
        best_excess = excess;
    }
    return best;
}

}  // namespace codec
