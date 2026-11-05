#include "codec/psychoacoustics.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace codec {
namespace {

constexpr std::size_t kBlockSize = 2048;
constexpr std::size_t kNyquistBin = kBlockSize / 2;
constexpr Sample kSampleRateHz = 48000;
constexpr Sample kBinWidthHz = kSampleRateHz / static_cast<Sample>(kBlockSize);

// A full-scale sine counts as this level. On the scale of the orthonormal MDCT, it has an energy of
// M / 2 per frame.
constexpr Sample kFullScaleDbSpl = 96;
constexpr Sample kFullScaleEnergy = 512;

// A spectral peak is tonal if it stands this far above its neighbors.
constexpr Sample kTonalPeakRatio = 5.0118723362727;  // 7 dB

// How far below a masker the masked noise stays, after Johnston: a tone masks noise less well than
// noise masks a tone.
constexpr Sample kNoiseMaskerOffsetDb = 5.5;
constexpr Sample kToneMaskerBaseOffsetDb = 14.5;

Sample decibels_to_ratio(Sample decibels) noexcept { return std::pow(10.0, decibels / 10); }

// Spreading function of Schroeder in dB, where distance is the Bark value of the masked band minus
// that of the masker. It falls by about 25 dB per Bark towards lower frequencies and by about 10 dB
// per Bark towards higher ones.
Sample spreading_db(Sample distance) noexcept {
    const Sample shifted = distance + 0.474;
    return 15.81 + 7.5 * shifted - 17.5 * std::sqrt(1 + shifted * shifted);
}

// The range of neighbors that a peak must stand out from grows with frequency, as critical bands
// get wider. These are the ranges of the MPEG-1 model, doubled for this longer block.
std::size_t tonal_neighbor_range(std::size_t bin) noexcept {
    if (bin < 126) {
        return 2;
    }
    if (bin < 254) {
        return 6;
    }
    if (bin < 510) {
        return 12;
    }
    return 24;
}

bool is_tonal_peak(const std::vector<Sample>& power, std::size_t bin) noexcept {
    if (power[bin] <= 0 || power[bin] < power[bin - 1] || power[bin] < power[bin + 1]) {
        return false;
    }
    const std::size_t range = tonal_neighbor_range(bin);
    for (std::size_t distance = 2; distance <= range; ++distance) {
        const Sample limit = power[bin] / kTonalPeakRatio;
        if (distance <= bin && power[bin - distance] > limit) {
            return false;
        }
        if (bin + distance <= kNyquistBin && power[bin + distance] > limit) {
            return false;
        }
    }
    return true;
}

// The band of a bin, for the bins of all bands. Bin k stands for coefficient k.
std::size_t band_of_bin(std::size_t bin) noexcept {
    std::size_t band = 0;
    while (band + 1 < kBandCount && bin >= kBandEdges[band + 1]) {
        ++band;
    }
    return band;
}

}  // namespace

Sample bark_from_hz(Sample frequency) noexcept {
    const Sample scaled = frequency / 7500;
    return 13 * std::atan(0.00076 * frequency) + 3.5 * std::atan(scaled * scaled);
}

Sample absolute_threshold_db_spl(Sample frequency) noexcept {
    const Sample khz = frequency / 1000;
    const Sample around_dip = khz - 3.3;
    return 3.64 * std::pow(khz, -0.8) - 6.5 * std::exp(-0.6 * around_dip * around_dip) +
           1e-3 * khz * khz * khz * khz;
}

PsychoacousticModel::PsychoacousticModel() : fft_(kBlockSize), window_(kBlockSize) {
    for (std::size_t n = 0; n < kBlockSize; ++n) {
        const Sample s = std::sin(kPi * (static_cast<Sample>(n) + 0.5) / kBlockSize);
        window_[n] = s * s;
    }

    for (std::size_t band = 0; band < kBandCount; ++band) {
        // Center of the band on the Bark scale, from the frequencies at both of its edges.
        const Sample low = static_cast<Sample>(kBandEdges[band]) * kBinWidthHz;
        const Sample high = static_cast<Sample>(kBandEdges[band + 1]) * kBinWidthHz;
        band_bark_[band] = (bark_from_hz(low) + bark_from_hz(high)) / 2;

        // The most sensitive frequency of the band decides, to stay on the safe side.
        Sample lowest_db =
            absolute_threshold_db_spl((static_cast<Sample>(kBandEdges[band]) + 0.5) * kBinWidthHz);
        for (std::size_t k = kBandEdges[band]; k < kBandEdges[band + 1]; ++k) {
            lowest_db = std::min(
                lowest_db, absolute_threshold_db_spl((static_cast<Sample>(k) + 0.5) * kBinWidthHz));
        }
        quiet_threshold_[band] = kFullScaleEnergy * decibels_to_ratio(lowest_db - kFullScaleDbSpl);
    }

    for (std::size_t maskee = 0; maskee < kBandCount; ++maskee) {
        Sample sum = 0;
        for (std::size_t masker = 0; masker < kBandCount; ++masker) {
            const Sample value =
                decibels_to_ratio(spreading_db(band_bark_[maskee] - band_bark_[masker]));
            spreading_[maskee][masker] = value;
            sum += value;
        }
        // Normalized, so that spreading does not raise the level of a signal with equal energy in
        // all bands. This lowers the threshold next to a single masker by a few dB, which errs on
        // the side of less noise.
        for (Sample& value : spreading_[maskee]) {
            value /= sum;
        }
    }
}

MaskingAnalysis PsychoacousticModel::analyze(const std::vector<Sample>& block) const {
    if (block.size() != kBlockSize) {
        throw std::invalid_argument("the psychoacoustic model needs a block of 2048 samples");
    }

    std::vector<Fft::Complex> spectrum(kBlockSize);
    for (std::size_t n = 0; n < kBlockSize; ++n) {
        spectrum[n] = Fft::Complex(window_[n] * block[n], 0);
    }
    fft_.transform(spectrum);

    // Scales the energy of bins 0 to N / 2 to that of the orthonormal MDCT: the Hann window keeps
    // 3/8 of the energy, and the FFT is N times too large. Only half of the bins are used, and the
    // block is two hops long, which gives 8 / (3 * N) in total.
    const Sample to_mdct_scale = 8.0 / (3.0 * static_cast<Sample>(kBlockSize));
    std::vector<Sample> power(kNyquistBin + 1);
    for (std::size_t k = 0; k <= kNyquistBin; ++k) {
        const Fft::Complex& value = spectrum[k];
        power[k] = (value.real() * value.real() + value.imag() * value.imag()) * to_mdct_scale;
    }

    MaskingAnalysis result;
    for (std::size_t band = 0; band < kBandCount; ++band) {
        for (std::size_t k = kBandEdges[band]; k < kBandEdges[band + 1]; ++k) {
            result.band_energy[band] += power[k];
        }
    }

    BandValues tonal_energy{};
    for (std::size_t k = 2; k + 1 <= kNyquistBin - 1; ++k) {
        if (is_tonal_peak(power, k)) {
            tonal_energy[band_of_bin(k)] += power[k - 1] + power[k] + power[k + 1];
        }
    }
    for (std::size_t band = 0; band < kBandCount; ++band) {
        result.tonality[band] =
            result.band_energy[band] > 0
                ? std::min(Sample{1}, tonal_energy[band] / result.band_energy[band])
                : Sample{0};
    }

    for (std::size_t maskee = 0; maskee < kBandCount; ++maskee) {
        Sample masked = 0;
        for (std::size_t masker = 0; masker < kBandCount; ++masker) {
            const Sample tonality = result.tonality[masker];
            const Sample offset_db = tonality * (kToneMaskerBaseOffsetDb + band_bark_[masker]) +
                                     (1 - tonality) * kNoiseMaskerOffsetDb;
            masked += result.band_energy[masker] * spreading_[maskee][masker] *
                      decibels_to_ratio(-offset_db);
        }
        result.masking_threshold[maskee] = std::max(masked, quiet_threshold_[maskee]);
    }
    return result;
}

}  // namespace codec
