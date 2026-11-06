#pragma once

#include <array>
#include <cstddef>
#include <vector>

#include "codec/bands.hpp"
#include "codec/fft.hpp"
#include "codec/types.hpp"

namespace codec {

using BandValues = std::array<Sample, kBandCount>;

/// Critical-band rate in Bark for a frequency in Hz.
Sample bark_from_hz(Sample frequency) noexcept;

/// Absolute threshold of hearing in dB SPL for a frequency in Hz, after Terhardt. The frequency
/// must be above 0.
Sample absolute_threshold_db_spl(Sample frequency) noexcept;

/// The result for one block. All energies are on the scale of the orthonormal MDCT coefficients, so
/// they can be compared directly with the sum of the squared coefficients of a band.
struct MaskingAnalysis {
    /// Energy of each scalefactor band.
    BandValues band_energy{};
    /// Share of the energy of each band that lies in tonal peaks, from 0 to 1.
    BandValues tonality{};
    /// Noise energy that each band can hold without it being heard.
    BandValues masking_threshold{};
    /// The part of masking_threshold that holds even without any masker. A band below this cannot
    /// be heard at all, whatever else the signal contains.
    BandValues quiet_threshold{};
};

/// The simplified psychoacoustic model from section 5 of spec/codec-spec.md, for 48 kHz.
class PsychoacousticModel {
public:
    PsychoacousticModel();

    /// Analyzes one block of 2048 samples, which must be the same samples as the MDCT block of the
    /// frame, so that both are aligned in time. Throws std::invalid_argument for any other size.
    MaskingAnalysis analyze(const std::vector<Sample>& block) const;

private:
    Fft fft_;
    std::vector<Sample> window_;
    BandValues band_bark_{};
    BandValues quiet_threshold_{};
    // spreading_[maskee][masker] is the share of the energy of the masker that reaches the maskee.
    std::array<BandValues, kBandCount> spreading_{};
};

}  // namespace codec
