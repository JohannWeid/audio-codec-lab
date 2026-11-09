#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "codec/allocation.hpp"
#include "codec/framing.hpp"
#include "codec/mdct.hpp"
#include "codec/psychoacoustics.hpp"
#include "codec/quantizer.hpp"
#include "test_helpers.hpp"

namespace {

using codec::band_noise;
using codec::BandValues;
using codec::encode_flat;
using codec::encode_perceptual;
using codec::encode_with_scalefactors;
using codec::EncodedFrame;
using codec::FrameCoderState;
using codec::FrameData;
using codec::kBandCount;
using codec::kBandEdges;
using codec::kFrameLength;
using codec::kMaxGain;
using codec::kMaxScalefactor;
using codec::kMinScalefactor;
using codec::kPi;
using codec::kReferenceGain;
using codec::MaskingAnalysis;
using codec::mean_excess_db;
using codec::Sample;
using codec::Scalefactors;
using codec::scalefactors_for_thresholds;

constexpr std::size_t kNarrowBandWidth = 4;  // The width of band 0.

// Three tones and a little noise, with one short burst of noise.
std::vector<Sample> music_like(std::size_t size) {
    const std::vector<Sample> noise = test::random_signal(size, 61);
    std::vector<Sample> signal(size);
    for (std::size_t n = 0; n < size; ++n) {
        const Sample t = static_cast<Sample>(n) / 48000;
        const Sample burst = (n / 12000) % 4 == 3 ? 0.15 : 0.005;
        signal[n] = 0.25 * std::sin(2 * kPi * 330 * t) + 0.12 * std::sin(2 * kPi * 2100 * t) +
                    0.05 * std::sin(2 * kPi * 7300 * t) + burst * noise[n];
    }
    return signal;
}

struct Measurement {
    Sample mean_excess = 0;
    Sample snr_db = 0;
    std::size_t largest_payload = 0;
};

template <typename EncodeFrame>
Measurement measure(const std::vector<Sample>& signal, EncodeFrame encode_frame) {
    const codec::Mdct mdct(kFrameLength);
    const codec::PsychoacousticModel model;
    const std::vector<std::vector<Sample>> blocks = codec::split_into_blocks(signal, kFrameLength);

    Measurement result;
    Sample signal_energy = 0;
    Sample noise_energy = 0;
    Sample counted = 0;
    FrameCoderState state;
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        const std::vector<Sample> coefficients = mdct.forward(blocks[i]);
        const MaskingAnalysis analysis = model.analyze(blocks[i]);
        const EncodedFrame encoded = encode_frame(coefficients, analysis, state);
        state = encoded.state;
        result.largest_payload = std::max(result.largest_payload, encoded.payload.size());
        // Only the frames in the middle count: the abrupt start and end of a test signal would
        // dominate the result in a signal of this length.
        if (i < 2 || i + 2 >= blocks.size()) {
            continue;
        }
        const BandValues noise = band_noise(coefficients, encoded.frame);
        result.mean_excess += mean_excess_db(noise, analysis.masking_threshold);
        signal_energy += test::energy(coefficients);
        for (const Sample value : noise) {
            noise_energy += value;
        }
        counted += 1;
    }
    result.mean_excess /= counted;
    result.snr_db = 10 * std::log10(signal_energy / noise_energy);
    return result;
}

Measurement measure_flat(const std::vector<Sample>& signal, std::size_t max_bytes) {
    return measure(signal, [max_bytes](const std::vector<Sample>& coefficients,
                                       const MaskingAnalysis&, const FrameCoderState& state) {
        return encode_flat(coefficients, max_bytes, state);
    });
}

Measurement measure_perceptual(const std::vector<Sample>& signal, std::size_t max_bytes) {
    return measure(signal,
                   [max_bytes](const std::vector<Sample>& coefficients,
                               const MaskingAnalysis& analysis, const FrameCoderState& state) {
                       return encode_perceptual(coefficients, analysis, max_bytes, state);
                   });
}

BandValues equal_values(Sample value) {
    BandValues values{};
    values.fill(value);
    return values;
}

}  // namespace

TEST(ScalefactorsForThresholds, GiveTheStepThatMatchesTheThreshold) {
    // A step s adds a noise of s^2 / 12 per coefficient, so 1/3 fits a step of 1 in 4 coefficients.
    const auto width = static_cast<Sample>(kNarrowBandWidth);
    BandValues thresholds = equal_values(1);
    thresholds[0] = width / 12;
    EXPECT_EQ(scalefactors_for_thresholds(thresholds)[0], 0);
    // Four times the noise needs twice the step, and a coarser step is a lower scalefactor.
    thresholds[0] = 4 * width / 12;
    EXPECT_EQ(scalefactors_for_thresholds(thresholds)[0], -8);
    thresholds[0] = width / 12 / 4;
    EXPECT_EQ(scalefactors_for_thresholds(thresholds)[0], 8);
}

TEST(ScalefactorsForThresholds, TakeTheWidthOfTheBandIntoAccount) {
    const Scalefactors scalefactors = scalefactors_for_thresholds(equal_values(1));
    // The same noise energy spread over more coefficients allows less noise per coefficient.
    EXPECT_GT(scalefactors[kBandCount - 1], scalefactors[0]);
}

TEST(ScalefactorsForThresholds, StayInRange) {
    for (const Sample value : {0.0, 1e-300, 1e300}) {
        SCOPED_TRACE(value);
        for (const int scalefactor : scalefactors_for_thresholds(equal_values(value))) {
            EXPECT_GE(scalefactor, kMinScalefactor);
            EXPECT_LE(scalefactor, kMaxScalefactor);
        }
    }
    EXPECT_EQ(scalefactors_for_thresholds(equal_values(0))[0], kMaxScalefactor);
    EXPECT_EQ(scalefactors_for_thresholds(equal_values(1e300))[0], kMinScalefactor);
}

// At the reference gain, the noise must really be close to the threshold. This is what connects the
// model to the quantizer.
TEST(ScalefactorsForThresholds, GiveNoiseNearTheThresholdAtTheReferenceGain) {
    const std::vector<Sample> coefficients = test::random_signal(kFrameLength, 62);
    const BandValues thresholds = equal_values(1e-3);
    const FrameData frame = codec::quantize_frame(coefficients, kReferenceGain,
                                                  scalefactors_for_thresholds(thresholds));
    const BandValues noise = band_noise(coefficients, frame);
    // Narrow bands vary a lot by chance, so this looks at the wide ones.
    for (std::size_t band = 16; band < kBandCount; ++band) {
        SCOPED_TRACE(band);
        EXPECT_NEAR(10 * std::log10(noise[band] / thresholds[band]), 0, 2);
    }
}

TEST(BandNoise, IsTheSquaredDifferencePerBand) {
    std::vector<Sample> coefficients(kFrameLength);
    coefficients[0] = 1.25;  // Band 0, step 1: decoded as 1, so the error is 0.25.
    coefficients[4] = -2.5;  // Band 1, step 1: decoded as -3, so the error is 0.5.
    const FrameData frame = codec::quantize_frame(coefficients, kReferenceGain, Scalefactors{});
    const BandValues noise = band_noise(coefficients, frame);
    EXPECT_DOUBLE_EQ(noise[0], 0.0625);
    EXPECT_DOUBLE_EQ(noise[1], 0.25);
    EXPECT_DOUBLE_EQ(noise[2], 0.0);
}

TEST(MeanExcess, CountsOnlyBandsAboveTheirThreshold) {
    BandValues noise = equal_values(1);
    EXPECT_DOUBLE_EQ(mean_excess_db(noise, equal_values(2)), 0.0);
    EXPECT_DOUBLE_EQ(mean_excess_db(noise, equal_values(1)), 0.0);
    // One band 10 dB above its threshold, out of 25.
    noise[3] = 20;
    EXPECT_NEAR(mean_excess_db(noise, equal_values(2)), 10.0 / 25, 1e-12);
}

TEST(InnerLoop, FindsTheSmallestGainThatFits) {
    const std::vector<Sample> coefficients = test::random_signal(kFrameLength, 63);
    const std::size_t max_bytes = 200;
    const EncodedFrame result = encode_flat(coefficients, max_bytes, FrameCoderState{});
    EXPECT_LE(result.payload.size(), max_bytes);
    ASSERT_GT(result.frame.gain, 0);

    FrameCoderState state;
    const std::vector<std::uint8_t> finer = codec::encode_frame_payload(
        codec::quantize_frame(coefficients, result.frame.gain - 1, Scalefactors{}), state);
    EXPECT_GT(finer.size(), max_bytes);
}

TEST(InnerLoop, UsesTheFinestStepIfEverythingFits) {
    const std::vector<Sample> coefficients(kFrameLength);
    EXPECT_EQ(encode_flat(coefficients, 100, FrameCoderState{}).frame.gain, 0);
}

// With very fine scalefactors, not even the highest gain empties the frame.
TEST(InnerLoop, FallsBackToAnEmptyFrameIfNothingElseFits) {
    Scalefactors finest{};
    finest.fill(kMaxScalefactor);
    const std::vector<Sample> coefficients(kFrameLength, 10.0);
    const EncodedFrame result =
        encode_with_scalefactors(coefficients, finest, 5, FrameCoderState{});
    EXPECT_LE(result.payload.size(), 5u);
    for (const std::int32_t value : result.frame.quantized) {
        EXPECT_EQ(value, 0);
    }
}

TEST(EncodePerceptual, KeepsNoiseBelowTheThresholdsWithEnoughBits) {
    const std::vector<Sample> signal = music_like(24 * kFrameLength);
    const Measurement result = measure_perceptual(signal, 1300);
    EXPECT_LT(result.mean_excess, 0.05);
}

// A band below the threshold in quiet cannot be heard in any case, so it needs no bits, however
// large the budget is. Those bits go to the other bands.
TEST(EncodePerceptual, LeavesInaudibleBandsEmptyEvenWithALargeBudget) {
    std::vector<Sample> coefficients(kFrameLength);
    coefficients[100] = 2.0;  // A strong coefficient far above its threshold.
    for (std::size_t k = 500; k < 600; ++k) {
        coefficients[k] = 1e-4;  // Weak content, below the threshold in quiet of its bands.
    }
    MaskingAnalysis analysis;
    analysis.masking_threshold = equal_values(1e-3);
    analysis.quiet_threshold = equal_values(1e-3);
    const EncodedFrame result = encode_perceptual(coefficients, analysis, 1000, FrameCoderState{});

    EXPECT_NE(result.frame.quantized[100], 0);
    for (std::size_t k = 500; k < 600; ++k) {
        EXPECT_EQ(result.frame.quantized[k], 0);
    }
    // The flat allocation has no thresholds and spends its bits on the weak content as well.
    const EncodedFrame flat = encode_flat(coefficients, 1000, FrameCoderState{});
    EXPECT_NE(flat.frame.quantized[550], 0);
}

// A band that is only masked by the rest of the signal is part of that signal, so it is coded if
// the budget allows it. Otherwise the quality of a tone would stop rising at some point, whatever
// the bitrate, because the skirts of its spectrum would be missing.
TEST(EncodePerceptual, CodesMaskedBandsIfTheBudgetAllowsIt) {
    std::vector<Sample> coefficients(kFrameLength);
    coefficients[100] = 2.0;
    for (std::size_t k = 500; k < 600; ++k) {
        coefficients[k] = 1e-3;  // Below its masking threshold, but above the threshold in quiet.
    }
    MaskingAnalysis analysis;
    analysis.masking_threshold = equal_values(1e-2);
    analysis.quiet_threshold = equal_values(1e-6);

    const EncodedFrame result = encode_perceptual(coefficients, analysis, 1000, FrameCoderState{});
    EXPECT_NE(result.frame.quantized[100], 0);
    EXPECT_NE(result.frame.quantized[550], 0);
}

// A tone is the easy case: it fits below its thresholds even at a low bitrate, with the bits going
// to the tone instead of the rounding noise of the input.
TEST(EncodePerceptual, ATonesNoiseStaysBelowItsThresholdAtALowBitrate) {
    const std::size_t size = 24 * kFrameLength;
    std::vector<Sample> signal(size);
    for (std::size_t n = 0; n < size; ++n) {
        // Rounded to 16 bits like real input, which adds the noise that no one can hear.
        signal[n] =
            std::round(0.25 * std::sin(2 * kPi * 1000 * static_cast<Sample>(n) / 48000) * 32768) /
            32768;
    }
    const Measurement at_32k = measure_perceptual(signal, 83);
    const Measurement at_96k = measure_perceptual(signal, 254);
    std::cout << "[allocation] tone: SNR " << at_32k.snr_db << " dB and excess "
              << at_32k.mean_excess << " dB at 83 bytes per frame, SNR " << at_96k.snr_db
              << " dB at 254 bytes\n";
    EXPECT_LT(at_32k.mean_excess, 0.01);
    EXPECT_GT(at_32k.snr_db, 25);
    EXPECT_GT(at_96k.snr_db, 55);
}

// With noise in all bands and few bits, the model does worse than flat noise by this measure: at 83
// bytes per frame, an excess of about 3.8 dB against 1.4 dB. That is the price of the wider
// bandwidth, accepted on purpose, see section 6 of the specification. With enough bits, the model
// must win.
TEST(EncodePerceptual, BeatsFlatNoiseForBroadbandSignalsWithEnoughBits) {
    const std::vector<Sample> white = test::random_signal(24 * kFrameLength, 64);
    std::vector<Sample> colored(white.size());
    Sample state = 0;
    for (std::size_t n = 0; n < white.size(); ++n) {
        state = 0.95 * state + 0.1 * white[n];
        colored[n] = state;
    }
    for (const std::size_t max_bytes : {std::size_t{83}, std::size_t{254}}) {
        const Measurement flat = measure_flat(colored, max_bytes);
        const Measurement perceptual = measure_perceptual(colored, max_bytes);
        std::cout << "[allocation] colored noise at " << max_bytes
                  << " bytes per frame: flat excess " << flat.mean_excess << " dB, SNR "
                  << flat.snr_db << " dB; perceptual excess " << perceptual.mean_excess
                  << " dB, SNR " << perceptual.snr_db << " dB\n";
        if (max_bytes == 254) {
            EXPECT_LT(perceptual.mean_excess, flat.mean_excess);
        }
    }
}

TEST(EncodePerceptual, LimitsTheBandwidthAtALowBitrate) {
    const std::vector<Sample> coefficients = test::random_signal(kFrameLength, 65);
    MaskingAnalysis analysis;
    analysis.masking_threshold = equal_values(1e-6);
    analysis.quiet_threshold = equal_values(1e-9);

    const auto highest_coded = [&](std::size_t max_bytes, std::uint32_t eighths = 8) {
        const EncodedFrame result =
            encode_perceptual(coefficients, analysis, max_bytes, FrameCoderState{}, eighths);
        std::size_t highest = 0;
        for (std::size_t k = 0; k < kFrameLength; ++k) {
            if (result.frame.quantized[k] != 0) {
                highest = k;
            }
        }
        return highest;
    };
    // At 1 bit per coefficient, 40 bytes are 320 bits, so the last complete band ends at edge 274.
    EXPECT_LT(highest_coded(40), 274u);
    // 127 bytes are 1016 bits, just too few for the highest band, which ends at 1024.
    EXPECT_LT(highest_coded(127), 658u);
    EXPECT_GE(highest_coded(127), 487u);
    // 128 bytes afford all coefficients, so nothing is cut off.
    EXPECT_GE(highest_coded(128), 658u);
    // The earlier limit of 1.75 bits, or 14 eighths: 83 bytes are 664 bits, which afford the bands
    // up to coefficient 379, so the last complete band ends at edge 325. At 1 bit, they reach 487
    // and more.
    EXPECT_LT(highest_coded(83, 14), 325u);
    EXPECT_GE(highest_coded(83, 8), 487u);
}

TEST(EncodePerceptual, RejectsABandwidthLimitOutOfRange) {
    const std::vector<Sample> coefficients = test::random_signal(kFrameLength, 66);
    MaskingAnalysis analysis;
    analysis.masking_threshold = equal_values(1e-6);
    analysis.quiet_threshold = equal_values(1e-9);
    for (const std::uint32_t eighths : {0u, 65u}) {
        SCOPED_TRACE(eighths);
        EXPECT_THROW(encode_perceptual(coefficients, analysis, 100, FrameCoderState{}, eighths),
                     std::invalid_argument);
    }
}

TEST(EncodePerceptual, NeverExceedsTheBudget) {
    const std::vector<Sample> signal = music_like(12 * kFrameLength);
    for (const std::size_t max_bytes : {std::size_t{19}, std::size_t{83}, std::size_t{168}}) {
        SCOPED_TRACE(max_bytes);
        EXPECT_LE(measure_perceptual(signal, max_bytes).largest_payload, max_bytes);
    }
}

// The purpose of the model: at the same bitrate, less noise lies above what the ear can hear, even
// though the SNR falls, as section 7 of the specification expects.
TEST(EncodePerceptual, KeepsMoreNoiseBelowTheThresholdsThanFlatNoise) {
    const std::vector<Sample> signal = music_like(48 * kFrameLength);
    for (const std::size_t max_bytes : {std::size_t{83}, std::size_t{124}, std::size_t{168}}) {
        SCOPED_TRACE(max_bytes);
        const Measurement flat = measure_flat(signal, max_bytes);
        const Measurement perceptual = measure_perceptual(signal, max_bytes);
        std::cout << "[allocation] " << max_bytes << " bytes per frame: flat excess "
                  << flat.mean_excess << " dB, SNR " << flat.snr_db << " dB; perceptual excess "
                  << perceptual.mean_excess << " dB, SNR " << perceptual.snr_db << " dB\n";
        EXPECT_LT(perceptual.mean_excess, flat.mean_excess);
        EXPECT_LT(perceptual.snr_db, flat.snr_db);
    }
}
