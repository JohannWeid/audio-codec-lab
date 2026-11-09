#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "codec/frame.hpp"
#include "codec/psychoacoustics.hpp"
#include "codec/types.hpp"

namespace codec {

/// At this gain, a band with the scalefactors of scalefactors_for_thresholds() has about as much
/// noise as its masking threshold allows. A lower gain stays below the threshold, a higher one
/// above.
inline constexpr int kReferenceGain = 144;

/// The bandwidth limit of the model, in eighths of a bit per coded coefficient: 8 is 1 bit. The
/// bands that the budget cannot afford at this rate stay empty. Section 6 of spec/codec-spec.md
/// explains the value. Eighths keep the computation in integers.
inline constexpr std::uint32_t kDefaultBandwidthLimitEighths = 8;
inline constexpr std::uint32_t kMinBandwidthLimitEighths = 1;
inline constexpr std::uint32_t kMaxBandwidthLimitEighths = 64;

/// Scalefactors that lead to a noise equal to the thresholds at kReferenceGain. They assume that a
/// quantizer with step s adds a noise energy of s^2 / 12 per coefficient.
Scalefactors scalefactors_for_thresholds(const BandValues& thresholds);

/// Energy of the quantization noise of each band: the squared difference between the coefficients
/// and what the decoder reconstructs from the frame.
BandValues band_noise(const std::vector<Sample>& coefficients, const FrameData& frame);

/// By how many dB the noise lies above the thresholds, as a mean over all bands. Bands with noise
/// below their threshold count as 0. Each band covers about one Bark, so they all count the same.
Sample mean_excess_db(const BandValues& noise, const BandValues& thresholds);

/// One coded frame, and the state of the coder after it, which the next frame continues from.
struct EncodedFrame {
    FrameData frame;
    std::vector<std::uint8_t> payload;
    FrameCoderState state;
};

/// The inner loop: the smallest gain whose payload fits, for fixed scalefactors. If not even the
/// highest gain fits, the frame stays empty, which always fits the smallest allowed bitrate.
EncodedFrame encode_with_scalefactors(const std::vector<Sample>& coefficients,
                                      const Scalefactors& scalefactors,
                                      std::size_t max_payload_bytes, const FrameCoderState& state);

/// Flat noise allocation, the baseline in section 7 of spec/codec-spec.md.
EncodedFrame encode_flat(const std::vector<Sample>& coefficients, std::size_t max_payload_bytes,
                         const FrameCoderState& state);

/// Noise allocation by the masking thresholds, as described in section 6 of spec/codec-spec.md.
/// Bands below the threshold in quiet stay empty, and so do the bands above the bandwidth that the
/// budget can afford with the given limit. Throws std::invalid_argument for a limit out of range.
EncodedFrame encode_perceptual(
    const std::vector<Sample>& coefficients, const MaskingAnalysis& analysis,
    std::size_t max_payload_bytes, const FrameCoderState& state,
    std::uint32_t bandwidth_limit_eighths = kDefaultBandwidthLimitEighths);

}  // namespace codec
