#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "codec/framing.hpp"
#include "codec/mdct.hpp"
#include "codec/psychoacoustics.hpp"
#include "test_helpers.hpp"

namespace {

using codec::absolute_threshold_db_spl;
using codec::BandValues;
using codec::bark_from_hz;
using codec::kBandCount;
using codec::kBandEdges;
using codec::kPi;
using codec::MaskingAnalysis;
using codec::PsychoacousticModel;
using codec::Sample;

constexpr std::size_t kBlockSize = 2048;
constexpr Sample kSampleRate = 48000;
constexpr Sample kBinWidth = 23.4375;

std::size_t band_of_frequency(Sample frequency) {
    const auto coefficient = static_cast<std::size_t>(frequency / kBinWidth);
    std::size_t band = 0;
    while (kBandEdges[band + 1] <= coefficient) {
        ++band;
    }
    return band;
}

std::vector<Sample> sine(Sample frequency, Sample amplitude, std::size_t size) {
    std::vector<Sample> signal(size);
    for (std::size_t n = 0; n < size; ++n) {
        signal[n] =
            amplitude * std::sin(2 * kPi * frequency * static_cast<Sample>(n) / kSampleRate);
    }
    return signal;
}

Sample db(Sample ratio) { return 10 * std::log10(ratio); }

const PsychoacousticModel& model() {
    static const PsychoacousticModel instance;
    return instance;
}

// The mean of each value over several blocks of a longer signal.
struct MeanAnalysis {
    BandValues energy{};
    BandValues tonality{};
    BandValues threshold{};
};

MeanAnalysis mean_analysis(const std::vector<Sample>& signal) {
    MeanAnalysis mean;
    Sample count = 0;
    for (std::size_t start = 0; start + kBlockSize <= signal.size(); start += kBlockSize / 2) {
        const std::vector<Sample> block(
            signal.begin() + static_cast<std::ptrdiff_t>(start),
            signal.begin() + static_cast<std::ptrdiff_t>(start + kBlockSize));
        const MaskingAnalysis result = model().analyze(block);
        for (std::size_t band = 0; band < kBandCount; ++band) {
            mean.energy[band] += result.band_energy[band];
            mean.tonality[band] += result.tonality[band];
            mean.threshold[band] += result.masking_threshold[band];
        }
        count += 1;
    }
    for (std::size_t band = 0; band < kBandCount; ++band) {
        mean.energy[band] /= count;
        mean.tonality[band] /= count;
        mean.threshold[band] /= count;
    }
    return mean;
}

// The mean energy per band and frame in the MDCT, without the frames at both ends.
BandValues mean_mdct_energy(const std::vector<Sample>& signal) {
    const codec::Mdct mdct(1024);
    const std::vector<std::vector<Sample>> frames = codec::analyze(signal, mdct);
    BandValues mean{};
    Sample count = 0;
    for (std::size_t i = 2; i + 2 < frames.size(); ++i) {
        for (std::size_t band = 0; band < kBandCount; ++band) {
            for (std::size_t k = kBandEdges[band]; k < kBandEdges[band + 1]; ++k) {
                mean[band] += frames[i][k] * frames[i][k];
            }
        }
        count += 1;
    }
    for (Sample& value : mean) {
        value /= count;
    }
    return mean;
}

Sample total(const BandValues& values) {
    Sample sum = 0;
    for (const Sample value : values) {
        sum += value;
    }
    return sum;
}

}  // namespace

TEST(Bark, MatchesKnownValues) {
    EXPECT_NEAR(bark_from_hz(0), 0, 1e-12);
    EXPECT_NEAR(bark_from_hz(100), 1.0, 0.1);
    EXPECT_NEAR(bark_from_hz(1000), 8.5, 0.1);
    EXPECT_NEAR(bark_from_hz(24000), 24.87, 0.01);
}

TEST(AbsoluteThreshold, HasTheKnownShape) {
    EXPECT_NEAR(absolute_threshold_db_spl(1000), 3.4, 0.2);
    EXPECT_NEAR(absolute_threshold_db_spl(100), 23, 1);
    // The ear is most sensitive between 3 and 4 kHz, where the threshold falls below 0 dB SPL.
    EXPECT_LT(absolute_threshold_db_spl(3300), -3);
    EXPECT_LT(absolute_threshold_db_spl(3300), absolute_threshold_db_spl(1000));
    EXPECT_LT(absolute_threshold_db_spl(3300), absolute_threshold_db_spl(8000));
    // It rises steeply towards the upper end of hearing.
    EXPECT_GT(absolute_threshold_db_spl(16000), 50);
}

// The model and the rate loop use different transforms. If their energy scales did not match, the
// noise would be compared with the wrong threshold.
TEST(PsychoacousticModel, EnergyScaleMatchesTheMdctForATone) {
    const std::vector<Sample> signal = sine(1000, 0.5, 40 * 1024);
    const Sample model_energy = total(mean_analysis(signal).energy);
    const Sample mdct_energy = total(mean_mdct_energy(signal));
    // A sine has an energy of M * amplitude^2 / 2 per frame. The energy in the MDCT varies a little
    // with the phase of the tone in each frame, so this holds only approximately.
    EXPECT_NEAR(db(mdct_energy / (1024 * 0.25 / 2)), 0, 0.1);
    EXPECT_NEAR(db(model_energy / mdct_energy), 0, 0.25);
}

TEST(PsychoacousticModel, EnergyScaleMatchesTheMdctInEveryBandForNoise) {
    std::vector<Sample> signal = test::random_signal(400 * 1024, 51);
    for (Sample& value : signal) {
        value *= 0.5;
    }
    const BandValues model_energy = mean_analysis(signal).energy;
    const BandValues mdct_energy = mean_mdct_energy(signal);
    for (std::size_t band = 0; band < kBandCount; ++band) {
        SCOPED_TRACE(band);
        EXPECT_NEAR(db(model_energy[band] / mdct_energy[band]), 0, 1.0);
    }
}

TEST(PsychoacousticModel, SilenceGivesTheThresholdInQuiet) {
    const MaskingAnalysis result = model().analyze(std::vector<Sample>(kBlockSize));
    for (std::size_t band = 0; band < kBandCount; ++band) {
        SCOPED_TRACE(band);
        EXPECT_EQ(result.band_energy[band], 0);
        EXPECT_EQ(result.tonality[band], 0);
        EXPECT_GT(result.masking_threshold[band], 0);
        const Sample center =
            (static_cast<Sample>(kBandEdges[band]) + static_cast<Sample>(kBandEdges[band + 1])) /
            2 * kBinWidth;
        // In quiet, the level follows the absolute threshold, on a scale where a full-scale sine
        // has an energy of 512 at 96 dB SPL. The lowest value of a band can only be below that of
        // its center.
        const Sample level_db = db(result.masking_threshold[band] / 512) + 96;
        EXPECT_LE(level_db, absolute_threshold_db_spl(center) + 0.01);
    }
    // At 1 kHz, a band is narrow enough for its threshold to be close to the curve.
    const std::size_t band = band_of_frequency(1000);
    EXPECT_NEAR(db(result.masking_threshold[band] / 512) + 96, absolute_threshold_db_spl(1000), 1);
}

TEST(PsychoacousticModel, ThresholdsAreFiniteForExtremeInput) {
    std::vector<Sample> square(kBlockSize);
    for (std::size_t n = 0; n < kBlockSize; ++n) {
        square[n] = n % 2 == 0 ? 1.0 : -1.0;
    }
    for (const std::vector<Sample>& block : {square, std::vector<Sample>(kBlockSize, 1.0)}) {
        const MaskingAnalysis result = model().analyze(block);
        for (std::size_t band = 0; band < kBandCount; ++band) {
            EXPECT_TRUE(std::isfinite(result.masking_threshold[band]));
            EXPECT_GE(result.tonality[band], 0);
            EXPECT_LE(result.tonality[band], 1);
        }
    }
}

TEST(PsychoacousticModel, RecognizesAToneAndNoise) {
    const std::size_t tone_band = band_of_frequency(1000);
    EXPECT_GT(mean_analysis(sine(1000, 0.5, 40 * 1024)).tonality[tone_band], 0.9);
    // A tone that does not fall on a bin of the FFT.
    const std::size_t other_band = band_of_frequency(4321);
    EXPECT_GT(mean_analysis(sine(4321, 0.5, 40 * 1024)).tonality[other_band], 0.9);

    // Single random peaks count as tonal now and then in the narrow low bands. In the high bands, a
    // peak has to stand out from many more bins, so this almost never happens there.
    const MeanAnalysis noise = mean_analysis(test::random_signal(200 * 1024, 52));
    for (std::size_t band = 0; band < kBandCount; ++band) {
        SCOPED_TRACE(band);
        EXPECT_LT(noise.tonality[band], band < 16 ? 0.3 : 0.05);
    }
}

TEST(PsychoacousticModel, FindsHighTonesInsideNoise) {
    std::vector<Sample> signal = test::random_signal(100 * 1024, 55);
    const std::vector<Sample> first = sine(9000, 0.2, signal.size());
    const std::vector<Sample> second = sine(15000, 0.2, signal.size());
    for (std::size_t n = 0; n < signal.size(); ++n) {
        signal[n] = 0.01 * signal[n] + first[n] + second[n];
    }
    const MeanAnalysis result = mean_analysis(signal);
    const std::size_t band_9k = band_of_frequency(9000);
    const std::size_t band_15k = band_of_frequency(15000);
    for (std::size_t band = 16; band < kBandCount; ++band) {
        SCOPED_TRACE(band);
        if (band == band_9k || band == band_15k) {
            EXPECT_GT(result.tonality[band], 0.9);
        } else {
            EXPECT_LT(result.tonality[band], 0.05);
        }
    }
}

TEST(PsychoacousticModel, AToneRaisesTheThresholdMoreTowardsHigherFrequencies) {
    const std::vector<Sample> signal = sine(1000, 0.5, 40 * 1024);
    const MeanAnalysis result = mean_analysis(signal);
    const MeanAnalysis quiet = mean_analysis(std::vector<Sample>(4 * 1024));
    const std::size_t tone = band_of_frequency(1000);

    // The tone lifts the threshold clearly above the threshold in quiet around itself.
    EXPECT_GT(db(result.threshold[tone] / quiet.threshold[tone]), 20);
    // Masking spreads further upwards than downwards.
    EXPECT_GT(result.threshold[tone + 3], result.threshold[tone - 3]);
    // Far below the tone, nothing is left of it.
    EXPECT_NEAR(db(result.threshold[1] / quiet.threshold[1]), 0, 0.1);
}

TEST(PsychoacousticModel, ThresholdStaysBelowTheMaskerByTheExpectedAmount) {
    const std::size_t tone_band = band_of_frequency(1000);
    const MeanAnalysis tone = mean_analysis(sine(1000, 0.5, 40 * 1024));
    // A tone at about 8.5 Bark masks noise down to about 23 dB below it.
    EXPECT_GT(db(tone.energy[tone_band] / tone.threshold[tone_band]), 23);
    EXPECT_LT(db(tone.energy[tone_band] / tone.threshold[tone_band]), 35);

    // Noise masks better: the threshold is only a few dB below it.
    std::vector<Sample> noise_signal = test::random_signal(200 * 1024, 53);
    for (Sample& value : noise_signal) {
        value *= 0.5;
    }
    const MeanAnalysis noise = mean_analysis(noise_signal);
    for (std::size_t band = 3; band < 20; ++band) {
        SCOPED_TRACE(band);
        const Sample smr = db(noise.energy[band] / noise.threshold[band]);
        EXPECT_GT(smr, 4);
        EXPECT_LT(smr, 10);
    }
}

TEST(PsychoacousticModel, ThresholdFollowsTheLevelOfTheMasker) {
    const std::size_t tone = band_of_frequency(1000);
    const Sample loud = mean_analysis(sine(1000, 0.5, 20 * 1024)).threshold[tone];
    const Sample soft = mean_analysis(sine(1000, 0.05, 20 * 1024)).threshold[tone];
    EXPECT_NEAR(db(loud / soft), 20, 0.5);
}

// The window is centered on the block, as the MDCT window is. A burst in the middle of the block
// therefore counts far more than the same burst at its start.
TEST(PsychoacousticModel, IsAlignedWithTheBlock) {
    std::vector<Sample> at_start(kBlockSize);
    std::vector<Sample> in_middle(kBlockSize);
    for (std::size_t n = 0; n < 64; ++n) {
        at_start[n] = 0.5;
        in_middle[kBlockSize / 2 - 32 + n] = 0.5;
    }
    EXPECT_GT(total(model().analyze(in_middle).band_energy),
              1000 * total(model().analyze(at_start).band_energy));
}

TEST(PsychoacousticModel, RejectsBlocksOfTheWrongSize) {
    EXPECT_THROW(model().analyze(std::vector<Sample>(1024)), std::invalid_argument);
    EXPECT_THROW(model().analyze(std::vector<Sample>(2049)), std::invalid_argument);
}

// Prints the model's view of a few signals into the test log, for a look at whether the values make
// sense in dB. It checks only that the values exist.
TEST(PsychoacousticModel, ReportsSignalToMaskRatios) {
    const struct {
        const char* name;
        std::vector<Sample> signal;
    } signals[] = {
        {"tone 1 kHz", sine(1000, 0.5, 40 * 1024)},
        {"noise", test::random_signal(100 * 1024, 54)},
    };
    for (const auto& s : signals) {
        const MeanAnalysis result = mean_analysis(s.signal);
        std::cout << "[psychoacoustics] " << s.name << " SMR in dB per band:";
        for (std::size_t band = 0; band < kBandCount; ++band) {
            const Sample smr = db(std::max(result.energy[band], 1e-30) / result.threshold[band]);
            std::cout << ' ' << std::round(smr);
            EXPECT_TRUE(std::isfinite(smr));
        }
        std::cout << '\n';
    }
}
