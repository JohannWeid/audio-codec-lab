#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "codec/wav.hpp"

namespace {

using Bytes = std::vector<std::uint8_t>;

void append(Bytes& out, const Bytes& more) { out.insert(out.end(), more.begin(), more.end()); }

Bytes tag(const char* text) {
    return {static_cast<std::uint8_t>(text[0]), static_cast<std::uint8_t>(text[1]),
            static_cast<std::uint8_t>(text[2]), static_cast<std::uint8_t>(text[3])};
}

Bytes u16(std::uint32_t value) {
    return {static_cast<std::uint8_t>(value & 0xFFu),
            static_cast<std::uint8_t>((value >> 8) & 0xFFu)};
}

Bytes u32(std::uint32_t value) {
    Bytes out = u16(value & 0xFFFFu);
    append(out, u16(value >> 16));
    return out;
}

Bytes chunk(const char* id, const Bytes& body) {
    Bytes out = tag(id);
    append(out, u32(static_cast<std::uint32_t>(body.size())));
    append(out, body);
    if (body.size() % 2 != 0) {
        out.push_back(0);
    }
    return out;
}

struct FmtFields {
    std::uint32_t format_tag = 1;
    std::uint32_t channels = 1;
    std::uint32_t sample_rate = 48000;
    std::uint32_t block_align = 2;
    std::uint32_t bits = 16;
};

Bytes fmt_chunk(const FmtFields& f = {}) {
    Bytes body = u16(f.format_tag);
    append(body, u16(f.channels));
    append(body, u32(f.sample_rate));
    append(body, u32(f.sample_rate * f.block_align));
    append(body, u16(f.block_align));
    append(body, u16(f.bits));
    return chunk("fmt ", body);
}

// Three samples: 1, -2 and 32767.
Bytes data_chunk() { return chunk("data", {0x01, 0x00, 0xFE, 0xFF, 0xFF, 0x7F}); }

Bytes riff(const std::vector<Bytes>& chunks) {
    Bytes body = tag("WAVE");
    for (const Bytes& c : chunks) {
        append(body, c);
    }
    Bytes out = tag("RIFF");
    append(out, u32(static_cast<std::uint32_t>(body.size())));
    append(out, body);
    return out;
}

// A file with a chunk of unknown type, so that the parser has several chunks to walk.
Bytes valid_file() { return riff({fmt_chunk(), chunk("LIST", {1, 2, 3}), data_chunk()}); }

const std::vector<std::int16_t> kExpectedSamples = {1, -2, 32767};

}  // namespace

TEST(WavParse, ReadsMinimalFile) {
    const codec::WavAudio audio = codec::parse_wav(riff({fmt_chunk(), data_chunk()}));
    EXPECT_EQ(audio.sample_rate, 48000u);
    EXPECT_EQ(audio.channels, 1u);
    EXPECT_EQ(audio.samples, kExpectedSamples);
}

TEST(WavParse, SkipsUnknownChunksIncludingPaddingByte) {
    // The LIST chunk has an odd size and is therefore followed by a padding byte. ffmpeg normally
    // writes such a chunk before the audio data.
    EXPECT_EQ(codec::parse_wav(valid_file()).samples, kExpectedSamples);
}

TEST(WavParse, IgnoresChunksAfterData) {
    const Bytes file = riff({fmt_chunk(), data_chunk(), chunk("LIST", {1, 2})});
    EXPECT_EQ(codec::parse_wav(file).samples, kExpectedSamples);
}

TEST(WavParse, AcceptsEmptyDataChunk) {
    EXPECT_TRUE(codec::parse_wav(riff({fmt_chunk(), chunk("data", {})})).samples.empty());
}

TEST(WavParse, AcceptsLongerFmtChunk) {
    Bytes body = u16(1);
    append(body, u16(1));
    append(body, u32(48000));
    append(body, u32(96000));
    append(body, u16(2));
    append(body, u16(16));
    append(body, u16(0));
    EXPECT_EQ(codec::parse_wav(riff({chunk("fmt ", body), data_chunk()})).samples,
              kExpectedSamples);
}

TEST(WavParse, KeepsChannelsInterleaved) {
    FmtFields stereo;
    stereo.channels = 2;
    stereo.block_align = 4;
    const Bytes file = riff({fmt_chunk(stereo), chunk("data", {1, 0, 2, 0, 3, 0, 4, 0})});
    const codec::WavAudio audio = codec::parse_wav(file);
    EXPECT_EQ(audio.channels, 2u);
    EXPECT_EQ(audio.samples, (std::vector<std::int16_t>{1, 2, 3, 4}));
}

TEST(WavParse, RejectsInvalidFiles) {
    FmtFields floating_point;
    floating_point.format_tag = 3;
    FmtFields extensible;
    extensible.format_tag = 0xFFFE;
    FmtFields bits24;
    bits24.bits = 24;
    bits24.block_align = 3;
    FmtFields no_channels;
    no_channels.channels = 0;
    FmtFields no_rate;
    no_rate.sample_rate = 0;
    FmtFields wrong_align;
    wrong_align.block_align = 4;
    FmtFields stereo;
    stereo.channels = 2;
    stereo.block_align = 4;

    Bytes wrong_riff = valid_file();
    wrong_riff[0] = 'X';
    Bytes wrong_wave = valid_file();
    wrong_wave[8] = 'X';

    // Size fields that promise more than the file holds, including the value that streaming writers
    // use for an unknown length.
    Bytes too_long = tag("data");
    append(too_long, u32(100));
    Bytes unknown_length = tag("data");
    append(unknown_length, u32(0xFFFFFFFFu));

    const struct {
        const char* name;
        Bytes bytes;
    } cases[] = {
        {"empty", {}},
        {"shorter than the RIFF header", {'R', 'I', 'F', 'F'}},
        {"not RIFF", wrong_riff},
        {"not WAVE", wrong_wave},
        {"no chunks", riff({})},
        {"no fmt", riff({data_chunk()})},
        {"no data", riff({fmt_chunk()})},
        {"data before fmt", riff({data_chunk(), fmt_chunk()})},
        {"fmt too short", riff({chunk("fmt ", {1, 0, 1, 0}), data_chunk()})},
        {"floating point", riff({fmt_chunk(floating_point), data_chunk()})},
        {"extensible", riff({fmt_chunk(extensible), data_chunk()})},
        {"24 bits", riff({fmt_chunk(bits24), data_chunk()})},
        {"no channels", riff({fmt_chunk(no_channels), data_chunk()})},
        {"sample rate 0", riff({fmt_chunk(no_rate), data_chunk()})},
        {"wrong block align", riff({fmt_chunk(wrong_align), data_chunk()})},
        {"half a sample", riff({fmt_chunk(), chunk("data", {1, 2, 3})})},
        {"half a stereo frame", riff({fmt_chunk(stereo), chunk("data", {1, 0})})},
        {"data longer than the file", riff({fmt_chunk(), too_long})},
        {"unknown data length", riff({fmt_chunk(), unknown_length})},
    };

    for (const auto& c : cases) {
        SCOPED_TRACE(c.name);
        EXPECT_THROW(codec::parse_wav(c.bytes), codec::WavError);
    }
}

// The sanitizer job in CI turns any read out of bounds in these two tests into a failure.
TEST(WavRobustness, EveryTruncationIsHandled) {
    const Bytes file = valid_file();
    for (std::size_t length = 0; length < file.size(); ++length) {
        const Bytes truncated(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(length));
        try {
            const codec::WavAudio audio = codec::parse_wav(truncated);
            EXPECT_EQ(audio.samples.size() % audio.channels, 0u);
        } catch (const codec::WavError&) {
            // Rejecting is fine. Any other exception fails the test.
        }
    }
}

TEST(WavRobustness, RandomDamageIsHandled) {
    const Bytes file = valid_file();
    std::mt19937 rng(12345);
    for (int round = 0; round < 20000; ++round) {
        Bytes damaged = file;
        const auto changes = static_cast<std::uint32_t>(1 + rng() % 4);
        for (std::uint32_t i = 0; i < changes; ++i) {
            damaged[rng() % damaged.size()] = static_cast<std::uint8_t>(rng() & 0xFFu);
        }
        try {
            const codec::WavAudio audio = codec::parse_wav(damaged);
            ASSERT_GT(audio.channels, 0u);
            EXPECT_EQ(audio.samples.size() % audio.channels, 0u);
        } catch (const codec::WavError&) {
            // Rejecting is fine. Any other exception fails the test.
        }
    }
}

TEST(WavSerialize, WritesExactCanonicalBytes) {
    codec::WavAudio audio;
    audio.sample_rate = 48000;
    audio.channels = 1;
    audio.samples = {1, -2, 32767};
    // clang-format off
    const Bytes expected = {
        'R',  'I',  'F',  'F',  42,   0,    0,    0,     // RIFF size: 36 + 6 data bytes
        'W',  'A',  'V',  'E',  'f',  'm',  't',  ' ',
        16,   0,    0,    0,    1,    0,    1,    0,     // fmt size, PCM, one channel
        0x80, 0xBB, 0,    0,    0,    0x77, 1,    0,     // 48000 Hz, 96000 bytes per second
        2,    0,    16,   0,    'd',  'a',  't',  'a',   // block align, bits, data tag
        6,    0,    0,    0,    0x01, 0x00, 0xFE, 0xFF,  // data size, samples 1 and -2
        0xFF, 0x7F,                                      // sample 32767
    };
    // clang-format on
    EXPECT_EQ(codec::serialize_wav(audio), expected);
}

TEST(WavSerialize, RoundtripKeepsExtremeValues) {
    codec::WavAudio audio;
    audio.sample_rate = 44100;
    audio.channels = 2;
    audio.samples = {-32768, 32767, 0, -1, 1, 256};
    const codec::WavAudio back = codec::parse_wav(codec::serialize_wav(audio));
    EXPECT_EQ(back.sample_rate, audio.sample_rate);
    EXPECT_EQ(back.channels, audio.channels);
    EXPECT_EQ(back.samples, audio.samples);
}

TEST(WavSerialize, RejectsInvalidAudio) {
    codec::WavAudio no_channels;
    no_channels.sample_rate = 48000;
    EXPECT_THROW(codec::serialize_wav(no_channels), codec::WavError);

    codec::WavAudio no_rate;
    no_rate.channels = 1;
    EXPECT_THROW(codec::serialize_wav(no_rate), codec::WavError);

    codec::WavAudio partial_frame;
    partial_frame.sample_rate = 48000;
    partial_frame.channels = 2;
    partial_frame.samples = {1, 2, 3};
    EXPECT_THROW(codec::serialize_wav(partial_frame), codec::WavError);

    codec::WavAudio byte_rate_overflow;
    byte_rate_overflow.sample_rate = 0xFFFFFFFFu;
    byte_rate_overflow.channels = 2;
    EXPECT_THROW(codec::serialize_wav(byte_rate_overflow), codec::WavError);
}

TEST(WavFile, RoundtripThroughFile) {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "audio_codec_lab_wav_roundtrip.wav";
    codec::WavAudio audio;
    audio.sample_rate = 48000;
    audio.channels = 1;
    audio.samples = {-32768, -1, 0, 1, 32767};

    codec::write_wav(path, audio);
    const codec::WavAudio back = codec::read_wav(path);
    std::filesystem::remove(path);

    EXPECT_EQ(back.sample_rate, audio.sample_rate);
    EXPECT_EQ(back.channels, audio.channels);
    EXPECT_EQ(back.samples, audio.samples);
}

TEST(WavFile, MissingFileThrows) {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "audio_codec_lab_file_that_does_not_exist.wav";
    EXPECT_THROW(codec::read_wav(path), codec::WavError);
}

TEST(WavFile, UnwritablePathThrows) {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "audio_codec_lab_missing_directory" / "out.wav";
    codec::WavAudio audio;
    audio.sample_rate = 48000;
    audio.channels = 1;
    EXPECT_THROW(codec::write_wav(path, audio), codec::WavError);
}
