#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "codec/framing.hpp"
#include "codec/pcm.hpp"
#include "test_helpers.hpp"

namespace {

using codec::analyze;
using codec::frame_count;
using codec::kPi;
using codec::Mdct;
using codec::Sample;
using codec::synthesize;
using Frames = std::vector<std::vector<Sample>>;

Sample frames_energy(const Frames& frames) {
    Sample sum = 0;
    for (const std::vector<Sample>& frame : frames) {
        sum += test::energy(frame);
    }
    return sum;
}

}  // namespace

TEST(Framing, FrameCountFollowsSpecification) {
    EXPECT_EQ(frame_count(0, 1024), 1u);
    EXPECT_EQ(frame_count(1, 1024), 2u);
    EXPECT_EQ(frame_count(1023, 1024), 2u);
    EXPECT_EQ(frame_count(1024, 1024), 2u);
    EXPECT_EQ(frame_count(1025, 1024), 3u);
    EXPECT_EQ(frame_count(2048, 1024), 3u);
    EXPECT_THROW(frame_count(1, 0), std::invalid_argument);
}

TEST(Framing, AnalyzeGivesExpectedShape) {
    const Mdct mdct(16);
    const Frames frames = analyze(test::random_signal(100, 4), mdct);
    ASSERT_EQ(frames.size(), frame_count(100, 16));
    for (const std::vector<Sample>& frame : frames) {
        EXPECT_EQ(frame.size(), 16u);
    }
}

// Covers lengths around a multiple of M, where the padding and the delay are easiest to get wrong.
TEST(Framing, ReconstructsSignalsOfAnyLength) {
    const struct {
        std::size_t m;
        std::vector<std::size_t> lengths;
    } cases[] = {
        {16, {0, 1, 15, 16, 17, 31, 32, 33, 100}},
        {1024, {1, 1023, 1024, 1025, 2048, 5000}},
    };
    for (const auto& c : cases) {
        const Mdct mdct(c.m);
        for (const std::size_t length : c.lengths) {
            SCOPED_TRACE(testing::Message() << "M=" << c.m << " length=" << length);
            const std::vector<Sample> signal = test::random_signal(length, 5);
            const std::vector<Sample> output = synthesize(analyze(signal, mdct), length, mdct);
            ASSERT_EQ(output.size(), length);
            EXPECT_LT(test::max_abs_difference(output, signal), 1e-12);
        }
    }
}

// The rate loop relies on these two properties to measure noise on the coefficients.
TEST(Framing, SignalEnergyIsTheSameInCoefficients) {
    const Mdct mdct(64);
    const std::vector<Sample> signal = test::random_signal(1000, 6);
    EXPECT_NEAR(frames_energy(analyze(signal, mdct)), test::energy(signal), 1e-10);
}

TEST(Framing, NoiseEnergyIsTheSameInTimeDomain) {
    const Mdct mdct(64);
    const std::vector<Sample> signal = test::random_signal(1000, 7);
    const Frames frames = analyze(signal, mdct);

    Frames noisy = frames;
    Sample coefficient_noise = 0;
    std::uint32_t seed = 100;
    for (std::vector<Sample>& frame : noisy) {
        const std::vector<Sample> noise = test::random_signal(frame.size(), seed++);
        for (std::size_t k = 0; k < frame.size(); ++k) {
            frame[k] += 0.01 * noise[k];
        }
        coefficient_noise += 0.01 * 0.01 * test::energy(noise);
    }

    // synthesize() cuts off the padded ends, but the noise spreads into them as well. So this
    // overlap-adds the difference of all blocks directly.
    std::vector<Sample> output_noise((frames.size() + 1) * 64);
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const std::vector<Sample> with_noise = mdct.inverse(noisy[i]);
        const std::vector<Sample> without_noise = mdct.inverse(frames[i]);
        for (std::size_t n = 0; n < with_noise.size(); ++n) {
            output_noise[i * 64 + n] += with_noise[n] - without_noise[n];
        }
    }
    EXPECT_NEAR(test::energy(output_noise), coefficient_noise, 1e-12);
}

// Confirms the frequency mapping that the scalefactor bands rely on: at 48 kHz with M = 1024,
// coefficient k is centered at (k + 1/2) * 23.4375 Hz.
TEST(Framing, ToneEndsUpInTheExpectedCoefficients) {
    constexpr std::size_t kM = 1024;
    constexpr std::size_t kBin = 100;
    const Sample frequency = (static_cast<Sample>(kBin) + 0.5) * 23.4375;
    std::vector<Sample> signal(8 * kM);
    for (std::size_t n = 0; n < signal.size(); ++n) {
        signal[n] = 0.5 * std::sin(2 * kPi * frequency * static_cast<Sample>(n) / 48000);
    }

    const Frames frames = analyze(signal, Mdct(kM));
    for (std::size_t i = 2; i < 6; ++i) {
        SCOPED_TRACE(i);
        const std::vector<Sample>& frame = frames[i];
        Sample near_tone = 0;
        for (std::size_t k = kBin - 4; k <= kBin + 4; ++k) {
            near_tone += frame[k] * frame[k];
        }
        EXPECT_GT(near_tone / test::energy(frame), 0.999);

        const auto largest = std::max_element(frame.begin(), frame.end(), [](Sample a, Sample b) {
            return std::fabs(a) < std::fabs(b);
        });
        const auto index = static_cast<std::size_t>(largest - frame.begin());
        EXPECT_GE(index, kBin - 1);
        EXPECT_LE(index, kBin + 1);
    }
}

TEST(Framing, SynthesizeRejectsWrongFrameCount) {
    const Mdct mdct(16);
    const Frames frames = analyze(test::random_signal(32, 8), mdct);
    EXPECT_THROW(synthesize(frames, 33, mdct), std::invalid_argument);
    EXPECT_THROW(synthesize(frames, 16, mdct), std::invalid_argument);
    // A length from a damaged file must not lead to a huge allocation.
    EXPECT_THROW(synthesize(frames, static_cast<std::size_t>(-1), mdct), std::invalid_argument);
}

TEST(Framing, SynthesizeRejectsWrongFrameSize) {
    const Mdct mdct(16);
    Frames frames = analyze(test::random_signal(32, 9), mdct);
    frames[1].pop_back();
    EXPECT_THROW(synthesize(frames, 32, mdct), std::invalid_argument);
}

// The whole path without quantization must give back the exact 16-bit values, which is why the test
// also contains the extreme values.
TEST(Framing, PcmSurvivesTheWholeTransformChainExactly) {
    std::vector<std::int16_t> pcm(5000);
    const std::vector<Sample> noise = test::random_signal(pcm.size(), 10);
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        pcm[i] = static_cast<std::int16_t>(std::floor(noise[i] * 32768));
    }
    pcm[0] = -32768;
    pcm[1] = 32767;
    pcm[2500] = -32768;
    pcm[2501] = 32767;
    pcm[4998] = -32768;
    pcm[4999] = 32767;

    const Mdct mdct(1024);
    const std::vector<Sample> output =
        synthesize(analyze(codec::pcm_to_samples(pcm), mdct), pcm.size(), mdct);
    EXPECT_EQ(codec::samples_to_pcm(output), pcm);
}
