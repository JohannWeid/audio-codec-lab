#include "codec/wav.hpp"

#include <cstddef>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>

namespace codec {
namespace {

constexpr std::uint16_t kFormatPcm = 1;
constexpr std::uint16_t kBitsPerSample = 16;
constexpr std::size_t kBytesPerSample = 2;
constexpr std::size_t kRiffHeaderSize = 12;
constexpr std::size_t kChunkHeaderSize = 8;
constexpr std::size_t kMinFmtSize = 16;

std::uint16_t read_u16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::uint32_t read_u32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

bool has_tag(const std::uint8_t* p, const char* tag) {
    return p[0] == static_cast<std::uint8_t>(tag[0]) && p[1] == static_cast<std::uint8_t>(tag[1]) &&
           p[2] == static_cast<std::uint8_t>(tag[2]) && p[3] == static_cast<std::uint8_t>(tag[3]);
}

void append_tag(std::vector<std::uint8_t>& out, const char* tag) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::uint8_t>(tag[i]));
    }
}

void append_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
}

void append_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    append_u16(out, static_cast<std::uint16_t>(value & 0xFFFFu));
    append_u16(out, static_cast<std::uint16_t>(value >> 16));
}

// Two's complement is written out, because the conversion to a signed type is only
// implementation-defined in C++17.
std::int16_t to_signed(std::uint16_t value) {
    const int wide = value;
    return static_cast<std::int16_t>(wide >= 0x8000 ? wide - 0x10000 : wide);
}

std::uint16_t to_unsigned(std::int16_t value) {
    const int wide = value;
    return static_cast<std::uint16_t>(wide < 0 ? wide + 0x10000 : wide);
}

struct Format {
    std::uint32_t sample_rate;
    std::uint16_t channels;
};

Format parse_format(const std::uint8_t* chunk, std::size_t size) {
    if (size < kMinFmtSize) {
        throw WavError("fmt chunk is too short");
    }
    const std::uint16_t format_tag = read_u16(chunk);
    const std::uint16_t channels = read_u16(chunk + 2);
    const std::uint32_t sample_rate = read_u32(chunk + 4);
    const std::uint16_t block_align = read_u16(chunk + 12);
    const std::uint16_t bits = read_u16(chunk + 14);

    if (format_tag != kFormatPcm) {
        throw WavError("only PCM is supported, found format tag " + std::to_string(format_tag));
    }
    if (bits != kBitsPerSample) {
        throw WavError("only 16-bit samples are supported, found " + std::to_string(bits));
    }
    if (channels == 0) {
        throw WavError("the file declares no channels");
    }
    if (sample_rate == 0) {
        throw WavError("the file declares a sample rate of 0");
    }
    if (block_align != channels * kBytesPerSample) {
        throw WavError("block align does not match the channel count");
    }
    return {sample_rate, channels};
}

}  // namespace

WavAudio parse_wav(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < kRiffHeaderSize || !has_tag(bytes.data(), "RIFF") ||
        !has_tag(bytes.data() + 8, "WAVE")) {
        throw WavError("not a RIFF/WAVE file");
    }

    bool have_format = false;
    Format format{};
    std::size_t pos = kRiffHeaderSize;

    // Walks the chunks instead of assuming a 44-byte header: ffmpeg, for example, normally writes a
    // LIST chunk before the audio data.
    while (bytes.size() - pos >= kChunkHeaderSize) {
        const std::uint8_t* header = bytes.data() + pos;
        const std::uint32_t size = read_u32(header + 4);
        pos += kChunkHeaderSize;
        if (size > bytes.size() - pos) {
            throw WavError("a chunk extends beyond the end of the file");
        }
        const std::uint8_t* body = bytes.data() + pos;

        if (has_tag(header, "fmt ")) {
            format = parse_format(body, size);
            have_format = true;
        } else if (has_tag(header, "data")) {
            if (!have_format) {
                throw WavError("the data chunk comes before the fmt chunk");
            }
            const std::size_t block_size = format.channels * kBytesPerSample;
            if (size % block_size != 0) {
                throw WavError("the data chunk does not hold a whole number of sample frames");
            }
            WavAudio audio;
            audio.sample_rate = format.sample_rate;
            audio.channels = format.channels;
            audio.samples.reserve(size / kBytesPerSample);
            for (std::size_t i = 0; i < size; i += kBytesPerSample) {
                audio.samples.push_back(to_signed(read_u16(body + i)));
            }
            return audio;
        }

        pos += size;
        // Chunks of odd size are followed by one padding byte, which the last chunk may lack.
        if (size % 2 != 0 && pos < bytes.size()) {
            ++pos;
        }
    }
    throw WavError(have_format ? "the file has no data chunk" : "the file has no fmt chunk");
}

std::vector<std::uint8_t> serialize_wav(const WavAudio& audio) {
    if (audio.channels == 0) {
        throw WavError("cannot write audio without channels");
    }
    if (audio.sample_rate == 0) {
        throw WavError("cannot write audio with a sample rate of 0");
    }
    if (audio.samples.size() % audio.channels != 0) {
        throw WavError("the samples do not fill a whole number of sample frames");
    }

    constexpr std::uint64_t kMax32 = std::numeric_limits<std::uint32_t>::max();
    constexpr std::uint64_t kMax16 = std::numeric_limits<std::uint16_t>::max();
    constexpr std::uint64_t kHeaderAfterRiffSize = 36;
    const std::uint64_t data_size = std::uint64_t{audio.samples.size()} * kBytesPerSample;
    const std::uint64_t block_align = std::uint64_t{audio.channels} * kBytesPerSample;
    const std::uint64_t byte_rate = std::uint64_t{audio.sample_rate} * block_align;
    if (data_size + kHeaderAfterRiffSize > kMax32 || block_align > kMax16 || byte_rate > kMax32) {
        throw WavError("the audio is too large for the WAV format");
    }

    std::vector<std::uint8_t> out;
    out.reserve(static_cast<std::size_t>(kRiffHeaderSize + kChunkHeaderSize + kMinFmtSize +
                                         kChunkHeaderSize + data_size));
    append_tag(out, "RIFF");
    append_u32(out, static_cast<std::uint32_t>(data_size + kHeaderAfterRiffSize));
    append_tag(out, "WAVE");

    append_tag(out, "fmt ");
    append_u32(out, static_cast<std::uint32_t>(kMinFmtSize));
    append_u16(out, kFormatPcm);
    append_u16(out, audio.channels);
    append_u32(out, audio.sample_rate);
    append_u32(out, static_cast<std::uint32_t>(byte_rate));
    append_u16(out, static_cast<std::uint16_t>(block_align));
    append_u16(out, kBitsPerSample);

    append_tag(out, "data");
    append_u32(out, static_cast<std::uint32_t>(data_size));
    for (const std::int16_t sample : audio.samples) {
        append_u16(out, to_unsigned(sample));
    }
    return out;
}

WavAudio read_wav(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw WavError("cannot open " + path.u8string());
    }
    const std::vector<char> raw((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
    if (file.bad()) {
        throw WavError("cannot read " + path.u8string());
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(raw.size());
    for (const char c : raw) {
        bytes.push_back(static_cast<std::uint8_t>(static_cast<unsigned char>(c)));
    }
    return parse_wav(bytes);
}

void write_wav(const std::filesystem::path& path, const WavAudio& audio) {
    const std::vector<std::uint8_t> bytes = serialize_wav(audio);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        throw WavError("cannot create " + path.u8string());
    }
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    file.close();
    if (file.fail()) {
        throw WavError("cannot write " + path.u8string());
    }
}

}  // namespace codec
