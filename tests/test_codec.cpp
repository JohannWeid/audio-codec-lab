#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <utility>
#include <vector>

#include "codec/codec.hpp"
#include "codec/framing.hpp"
#include "test_helpers.hpp"

namespace {

using Bytes = std::vector<std::uint8_t>;
using Pcm = std::vector<std::int16_t>;
using codec::BitstreamError;
using codec::decode;
using codec::encode;
using codec::EncoderOptions;
using codec::frame_budget_bits;
using codec::kFrameLength;
using codec::kPi;
using codec::kSampleRate;
using codec::parse_stream;
using codec::Sample;
using codec::WavAudio;

constexpr std::size_t kHeaderSize = 20;

WavAudio make_audio(Pcm samples) {
    WavAudio audio;
    audio.sample_rate = kSampleRate;
    audio.channels = 1;
    audio.samples = std::move(samples);
    return audio;
}

// Flat noise by default, since the tests of SNR values only make sense for the flat allocation.
Bytes encode_at(const Pcm& samples, std::uint32_t bitrate, bool psychoacoustics = false) {
    EncoderOptions options;
    options.bitrate = bitrate;
    options.psychoacoustics = psychoacoustics;
    return encode(make_audio(samples), options);
}

Pcm tone(Sample frequency, Sample amplitude, std::size_t size) {
    Pcm samples(size);
    for (std::size_t n = 0; n < size; ++n) {
        const Sample phase = 2 * kPi * frequency * static_cast<Sample>(n) / kSampleRate;
        samples[n] = static_cast<std::int16_t>(std::lround(amplitude * 32767 * std::sin(phase)));
    }
    return samples;
}

Pcm noise(Sample amplitude, std::size_t size, std::uint32_t seed) {
    const std::vector<Sample> values = test::random_signal(size, seed);
    Pcm samples(size);
    for (std::size_t n = 0; n < size; ++n) {
        samples[n] = static_cast<std::int16_t>(std::lround(amplitude * 32767 * values[n]));
    }
    return samples;
}

// Several tones with a little noise, as a rough stand-in for music.
Pcm mixture(std::size_t size) {
    const Pcm a = tone(220, 0.3, size);
    const Pcm b = tone(1250, 0.2, size);
    const Pcm c = tone(5100, 0.1, size);
    const Pcm d = noise(0.02, size, 41);
    Pcm samples(size);
    for (std::size_t n = 0; n < size; ++n) {
        samples[n] = static_cast<std::int16_t>(a[n] + b[n] + c[n] + d[n]);
    }
    return samples;
}

Pcm full_scale_square(std::size_t size) {
    Pcm samples(size);
    for (std::size_t n = 0; n < size; ++n) {
        samples[n] = (n / 37) % 2 == 0 ? 32767 : -32768;
    }
    return samples;
}

Sample snr_db(const Pcm& original, const Pcm& decoded) {
    Sample signal = 0;
    Sample error = 0;
    for (std::size_t n = 0; n < original.size(); ++n) {
        const Sample a = original[n];
        const Sample difference = a - static_cast<Sample>(decoded[n]);
        signal += a * a;
        error += difference * difference;
    }
    return 10 * std::log10(signal / error);
}

// Leaves out two frames at each end. A test signal starts and stops abruptly, which spreads over
// all frequencies, and in a short signal those few frames would dominate the result.
Sample middle_snr_db(const Pcm& original, const Pcm& decoded) {
    const auto skip = static_cast<std::ptrdiff_t>(2 * kFrameLength);
    return snr_db(Pcm(original.begin() + skip, original.end() - skip),
                  Pcm(decoded.begin() + skip, decoded.end() - skip));
}

// The total sizes in bits of all frames, read from the length fields.
std::vector<std::uint32_t> frame_bits(const Bytes& stream) {
    std::vector<std::uint32_t> sizes;
    std::size_t position = kHeaderSize;
    while (position < stream.size()) {
        const std::size_t payload = (std::size_t{stream[position]} << 8) | stream[position + 1];
        sizes.push_back(static_cast<std::uint32_t>(16 + 8 * payload));
        position += 2 + payload;
    }
    return sizes;
}

void put_u32(Bytes& bytes, std::size_t position, std::uint32_t value) {
    bytes[position] = static_cast<std::uint8_t>(value >> 24);
    bytes[position + 1] = static_cast<std::uint8_t>((value >> 16) & 0xFFu);
    bytes[position + 2] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
    bytes[position + 3] = static_cast<std::uint8_t>(value & 0xFFu);
}

}  // namespace

TEST(FrameBudget, FollowsTheSpecification) {
    const std::uint32_t expected_32k[] = {682, 683, 683, 682, 683, 683};
    const std::uint32_t expected_64k[] = {1365, 1365, 1366, 1365, 1365, 1366};
    for (std::uint64_t n = 0; n < 6; ++n) {
        EXPECT_EQ(frame_budget_bits(n, 32000), expected_32k[n]);
        EXPECT_EQ(frame_budget_bits(n, 48000), 1024u);
        EXPECT_EQ(frame_budget_bits(n, 64000), expected_64k[n]);
    }
}

TEST(FrameBudget, SumFollowsTheBitrateExactly) {
    for (const std::uint32_t bitrate : {8000u, 32000u, 44100u, 64000u, 512000u}) {
        SCOPED_TRACE(bitrate);
        std::uint64_t sum = 0;
        for (std::uint64_t n = 0; n < 4500; ++n) {
            sum += frame_budget_bits(n, bitrate);
        }
        EXPECT_EQ(sum, std::uint64_t{bitrate} * 4500 * 1024 / 48000);
    }
}

TEST(Stream, HasTheSpecifiedHeaderAndEmptyFrame) {
    // No samples still give one frame, without any data: 25 empty bands.
    // clang-format off
    const Bytes expected = {
        'A', 'C', 'L', 'B',            // magic
        1,                             // version
        0x00, 0x00, 0xBB, 0x80,        // 48000 Hz
        1,                             // one channel
        0x04, 0x00,                    // 1024 samples per frame
        0, 0, 0, 0, 0, 0, 0, 0,        // no samples
        0x00, 0x05,                    // payload length
        0x00, 0xFF, 0xFF, 0xFF, 0x80,  // gain 0, 25 ones, padding
    };
    // clang-format on
    EXPECT_EQ(encode_at({}, 64000), expected);
}

TEST(Stream, HeaderHoldsTheSampleCount) {
    const Bytes stream = encode_at(Pcm(1000), 64000);
    EXPECT_EQ(stream[18], 0x03);
    EXPECT_EQ(stream[19], 0xE8);
}

TEST(Encoder, RejectsWhatTheSpecificationExcludes) {
    WavAudio other_rate = make_audio(Pcm(100));
    other_rate.sample_rate = 44100;
    EXPECT_THROW(encode(other_rate, EncoderOptions{}), std::invalid_argument);

    WavAudio stereo = make_audio(Pcm(100));
    stereo.channels = 2;
    EXPECT_THROW(encode(stereo, EncoderOptions{}), std::invalid_argument);

    EXPECT_THROW(encode_at(Pcm(100), codec::kMinBitrate - 1), std::invalid_argument);
    EXPECT_THROW(encode_at(Pcm(100), codec::kMaxBitrate + 1), std::invalid_argument);
}

TEST(Encoder, GivesTheSameBitstreamForTheSameInput) {
    const Pcm input = mixture(8000);
    for (const bool psychoacoustics : {false, true}) {
        EXPECT_EQ(encode_at(input, 48000, psychoacoustics),
                  encode_at(input, 48000, psychoacoustics));
    }
}

TEST(Encoder, NeverExceedsTheBudgetOfAFrame) {
    const std::size_t size = 8 * kFrameLength;
    const Pcm signals[] = {noise(0.9, size, 42), mixture(size), tone(1000, 0.9, size),
                           full_scale_square(size)};
    for (const Pcm& signal : signals) {
        for (const std::uint32_t bitrate : {8000u, 32000u, 64000u, 512000u}) {
            for (const bool psychoacoustics : {false, true}) {
                SCOPED_TRACE(testing::Message() << bitrate << " psy=" << psychoacoustics);
                const std::vector<std::uint32_t> sizes =
                    frame_bits(encode_at(signal, bitrate, psychoacoustics));
                ASSERT_EQ(sizes.size(), codec::frame_count(size, kFrameLength));
                for (std::size_t i = 0; i < sizes.size(); ++i) {
                    ASSERT_LE(sizes[i], frame_budget_bits(i, bitrate));
                }
            }
        }
    }
}

// Only from 64 kbit/s on: a Rice code needs at least one bit per coefficient in every band that is
// not empty, so below about one bit per sample a dense signal cannot use its whole budget.
TEST(Encoder, UsesMostOfTheBudgetForADenseSignal) {
    const std::size_t size = 16 * kFrameLength;
    for (const std::uint32_t bitrate : {64000u, 128000u, 256000u}) {
        SCOPED_TRACE(bitrate);
        const std::vector<std::uint32_t> sizes =
            frame_bits(encode_at(noise(0.5, size, 43), bitrate));
        std::uint64_t used = 0;
        std::uint64_t budget = 0;
        for (std::size_t i = 0; i < sizes.size(); ++i) {
            used += sizes[i];
            budget += frame_budget_bits(i, bitrate);
        }
        EXPECT_GT(static_cast<Sample>(used) / static_cast<Sample>(budget), 0.9);
    }
}

TEST(Codec, OutputHasExactlyTheLengthOfTheInput) {
    for (const std::size_t size :
         {std::size_t{0}, std::size_t{1}, std::size_t{1023}, std::size_t{1024}, std::size_t{1025},
          std::size_t{4096}, std::size_t{10000}}) {
        for (const bool psychoacoustics : {false, true}) {
            SCOPED_TRACE(testing::Message() << size << " psy=" << psychoacoustics);
            const WavAudio output = decode(encode_at(noise(0.5, size, 44), 64000, psychoacoustics));
            EXPECT_EQ(output.samples.size(), size);
            EXPECT_EQ(output.sample_rate, kSampleRate);
            EXPECT_EQ(output.channels, 1u);
        }
    }
}

TEST(Codec, SilenceStaysExactlySilent) {
    const Pcm silence(5000);
    for (const bool psychoacoustics : {false, true}) {
        SCOPED_TRACE(psychoacoustics);
        const Bytes stream = encode_at(silence, 64000, psychoacoustics);
        EXPECT_EQ(decode(stream).samples, silence);
        // Every frame holds only its length field and five bytes of payload.
        EXPECT_EQ(stream.size(), kHeaderSize + codec::frame_count(5000, kFrameLength) * 7);
    }
}

TEST(Codec, OutputLinesUpWithTheInput) {
    Pcm input(6000);
    input[3210] = 30000;
    for (const bool psychoacoustics : {false, true}) {
        SCOPED_TRACE(psychoacoustics);
        const Pcm output = decode(encode_at(input, 256000, psychoacoustics)).samples;
        const auto largest = std::max_element(output.begin(), output.end());
        EXPECT_EQ(largest - output.begin(), 3210);
    }
}

TEST(Codec, QualityGrowsWithTheBitrate) {
    const Pcm input = mixture(16 * kFrameLength);
    Sample previous = -1000;
    for (const std::uint32_t bitrate : {16000u, 32000u, 64000u, 128000u, 256000u}) {
        SCOPED_TRACE(bitrate);
        const Sample snr = snr_db(input, decode(encode_at(input, bitrate)).samples);
        EXPECT_GT(snr, previous);
        previous = snr;
    }
}

// Guards against regressions, with a margin below the values measured with the Rice code. A better
// entropy coder in phase 3 should raise them.
TEST(Codec, ReachesExpectedQuality) {
    const std::size_t size = 16 * kFrameLength;
    const Pcm a_tone = tone(1000, 0.5, size);
    const Pcm white = noise(0.5, size, 45);
    const Pcm mixed = mixture(size);
    EXPECT_GT(middle_snr_db(a_tone, decode(encode_at(a_tone, 64000)).samples), 60);
    // The tones of this signal are about 27 dB above its noise. The Rice code cannot afford to
    // transmit that noise at this bitrate, so the SNR stays near this value.
    EXPECT_GT(middle_snr_db(mixed, decode(encode_at(mixed, 64000)).samples), 25);
    EXPECT_GT(middle_snr_db(white, decode(encode_at(white, 64000)).samples), 1.5);
    EXPECT_GT(middle_snr_db(white, decode(encode_at(white, 512000)).samples), 45);
}

// The frames at the ends of a signal with an abrupt start and stop hold broadband content and so
// reach a lower SNR. If the middle were as bad as the whole signal, the cause would be elsewhere.
TEST(Codec, AbruptSignalEndsLowerTheOverallSnrOnly) {
    const Pcm a_tone = tone(1000, 0.5, 16 * kFrameLength);
    const Pcm output = decode(encode_at(a_tone, 64000)).samples;
    EXPECT_GT(middle_snr_db(a_tone, output), snr_db(a_tone, output) + 6);
}

TEST(Codec, HandlesAFullScaleSignalAtTheLowestBitrate) {
    const Pcm input = full_scale_square(4096);
    for (const bool psychoacoustics : {false, true}) {
        SCOPED_TRACE(psychoacoustics);
        const WavAudio output = decode(encode_at(input, codec::kMinBitrate, psychoacoustics));
        EXPECT_EQ(output.samples.size(), input.size());
    }
}

TEST(Decoder, ParseGivesTheSameIntegersAsTheEncoderUsed) {
    const Pcm input = mixture(5000);
    const codec::ParsedStream stream = parse_stream(encode_at(input, 64000));
    EXPECT_EQ(stream.total_samples, 5000u);
    EXPECT_EQ(stream.frames.size(), codec::frame_count(5000, kFrameLength));
    for (const codec::FrameData& frame : stream.frames) {
        EXPECT_EQ(frame.quantized.size(), kFrameLength);
    }
}

TEST(Decoder, RejectsDamagedHeaders) {
    const Bytes valid = encode_at(mixture(3000), 32000);

    Bytes wrong_magic = valid;
    wrong_magic[0] = 'X';
    Bytes wrong_version = valid;
    wrong_version[4] = 2;
    Bytes wrong_rate = valid;
    wrong_rate[8] = 0x44;
    Bytes wrong_channels = valid;
    wrong_channels[9] = 2;
    Bytes wrong_frame_length = valid;
    wrong_frame_length[10] = 0x08;
    Bytes more_samples = valid;
    put_u32(more_samples, 16, 3000 + 2 * kFrameLength);
    Bytes fewer_samples = valid;
    put_u32(fewer_samples, 16, 10);
    Bytes huge_samples = valid;
    put_u32(huge_samples, 12, 0xFFFFFFFFu);
    put_u32(huge_samples, 16, 0xFFFFFFFFu);
    Bytes high_samples = valid;
    put_u32(high_samples, 12, 1);
    Bytes with_trailing_data = valid;
    with_trailing_data.push_back(0);

    const struct {
        const char* name;
        Bytes bytes;
    } cases[] = {
        {"empty", {}},
        {"wrong magic", wrong_magic},
        {"wrong version", wrong_version},
        {"wrong sample rate", wrong_rate},
        {"wrong channel count", wrong_channels},
        {"wrong frame length", wrong_frame_length},
        {"more samples than frames", more_samples},
        {"fewer samples than frames", fewer_samples},
        {"largest sample count", huge_samples},
        {"sample count beyond 32 bits", high_samples},
        {"data after the last frame", with_trailing_data},
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(c.name);
        EXPECT_THROW(decode(c.bytes), BitstreamError);
    }
}

TEST(Decoder, RejectsAFrameLongerThanTheFile) {
    Bytes stream = encode_at(mixture(1000), 32000);
    stream[kHeaderSize] = 0xFF;
    stream[kHeaderSize + 1] = 0xFF;
    EXPECT_THROW(decode(stream), BitstreamError);
}

TEST(Decoder, RejectsAPayloadWithUnusedData) {
    // A frame that claims one byte more than its content needs, with the rest of the file behind
    // it.
    Bytes stream = encode_at(Pcm(100), 64000);
    const std::size_t payload = stream[kHeaderSize + 1];
    stream[kHeaderSize + 1] = static_cast<std::uint8_t>(payload + 1);
    stream.insert(stream.begin() + static_cast<std::ptrdiff_t>(kHeaderSize + 2 + payload), 0);
    EXPECT_THROW(decode(stream), BitstreamError);
}

TEST(Decoder, RejectsEveryTruncation) {
    const Bytes stream = encode_at(mixture(1500), 16000);
    for (std::size_t size = 0; size < stream.size(); ++size) {
        SCOPED_TRACE(size);
        const Bytes truncated(stream.begin(), stream.begin() + static_cast<std::ptrdiff_t>(size));
        EXPECT_THROW(decode(truncated), BitstreamError);
    }
}

// The sanitizer job in CI turns any read out of bounds or undefined behavior into a failure. Damage
// may also lead to a stream that still parses, and then the output must have the announced length.
TEST(Decoder, RandomDamageIsHandled) {
    const Bytes stream = encode_at(mixture(3000), 32000);
    std::mt19937 rng(46);
    for (int round = 0; round < 3000; ++round) {
        Bytes damaged = stream;
        const auto changes = static_cast<std::uint32_t>(1 + rng() % 8);
        for (std::uint32_t i = 0; i < changes; ++i) {
            damaged[rng() % damaged.size()] = static_cast<std::uint8_t>(rng() & 0xFFu);
        }
        try {
            const std::uint64_t announced = parse_stream(damaged).total_samples;
            ASSERT_EQ(decode(damaged).samples.size(), announced);
        } catch (const BitstreamError&) {
            // Rejecting is fine. Any other exception fails the test.
        }
    }
}
