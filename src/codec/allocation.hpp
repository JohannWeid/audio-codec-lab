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

/// Scalefactors that lead to a noise equal to the thresholds at kReferenceGain. They assume that a
/// quantizer with step s adds a noise energy of s^2 / 12 per coefficient.
Scalefactors scalefactors_for_thresholds(const BandValues& thresholds);

/// Energy of the quantization noise of each band: the squared difference between the coefficients
/// and what the decoder reconstructs from the frame.
BandValues band_noise(const std::vector<Sample>& coefficients, const FrameData& frame);

/// By how many dB the noise lies above the thresholds, as a mean over all bands. Bands with noise
/// below their threshold count as 0. Each band covers about one Bark, so they all count the same.
Sample mean_excess_db(const BandValues& noise, const BandValues& thresholds);

struct EncodedFrame {
    FrameData frame;
    std::vector<std::uint8_t> payload;
};

/// The inner loop: the smallest gain whose payload fits, for fixed scalefactors. If not even the
/// highest gain fits, the frame stays empty, which always fits the smallest allowed bitrate.
EncodedFrame encode_with_scalefactors(const std::vector<Sample>& coefficients,
                                      const Scalefactors& scalefactors,
                                      std::size_t max_payload_bytes);

/// Flat noise allocation, the baseline in section 7 of spec/codec-spec.md.
EncodedFrame encode_flat(const std::vector<Sample>& coefficients, std::size_t max_payload_bytes);

/// Noise allocation by the masking thresholds, with the outer loop in section 6 of
/// spec/codec-spec.md. The outer loop stops as soon as it no longer lowers mean_excess_db(). Bands
/// below the threshold in quiet stay empty.
EncodedFrame encode_perceptual(const std::vector<Sample>& coefficients,
                               const MaskingAnalysis& analysis, std::size_t max_payload_bytes);

}  // namespace codec
