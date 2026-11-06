#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include "codec/codec.hpp"
#include "codec/wav.hpp"

namespace {

using Bytes = std::vector<std::uint8_t>;
using Pcm = std::vector<std::int16_t>;

// The tolerance for the PCM output in section 4 of spec/codec-spec.md.
constexpr int kToleranceLsb = 1;

// How far the SNR of a bitstream encoded on this platform may be from that of the reference.
constexpr double kSnrToleranceDb = 0.5;

const struct Reference {
    const char* name;
    std::uint32_t bitrate;
    bool psychoacoustics;
    std::uint64_t integer_hash;
} kReferences[] = {
    {"ref_32k", 32000, false, 0x7A5D27778AC6445Du},
    {"ref_64k", 64000, false, 0x3EEB90D2AF8BAB31u},
    {"ref_psy_64k", 64000, true, 0xF5D74F03DFAAA85Bu},
};

std::filesystem::path data_path(const std::string& file) {
    return std::filesystem::path(CODEC_CONFORMANCE_DIR) / file;
}

Bytes read_bytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("cannot open " + path.u8string());
    }
    const std::vector<char> raw((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
    Bytes bytes;
    bytes.reserve(raw.size());
    for (const char c : raw) {
        bytes.push_back(static_cast<std::uint8_t>(static_cast<unsigned char>(c)));
    }
    return bytes;
}

// FNV-1a over 32-bit values, each taken as four bytes with the lowest byte first.
class IntegerHash {
public:
    void add(std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) {
            hash_ ^= (value >> shift) & 0xFFu;
            hash_ *= 0x100000001B3u;
        }
    }
    std::uint64_t value() const { return hash_; }

private:
    std::uint64_t hash_ = 0xCBF29CE484222325u;
};

std::uint64_t hash_of(const codec::ParsedStream& stream) {
    IntegerHash hash;
    hash.add(static_cast<std::uint32_t>(stream.total_samples));
    hash.add(static_cast<std::uint32_t>(stream.frames.size()));
    for (const codec::FrameData& frame : stream.frames) {
        hash.add(static_cast<std::uint32_t>(frame.gain));
        for (const int scalefactor : frame.scalefactors) {
            hash.add(static_cast<std::uint32_t>(scalefactor));
        }
        for (const std::int32_t value : frame.quantized) {
            hash.add(static_cast<std::uint32_t>(value));
        }
    }
    return hash.value();
}

double snr_db(const Pcm& original, const Pcm& decoded) {
    double signal = 0;
    double error = 0;
    for (std::size_t n = 0; n < original.size(); ++n) {
        const double a = original[n];
        const double difference = a - static_cast<double>(decoded[n]);
        signal += a * a;
        error += difference * difference;
    }
    return 10 * std::log10(signal / error);
}

}  // namespace

TEST(Conformance, DecodedIntegersAreBitExact) {
    for (const Reference& reference : kReferences) {
        SCOPED_TRACE(reference.name);
        const codec::ParsedStream stream =
            codec::parse_stream(read_bytes(data_path(std::string(reference.name) + ".bin")));
        EXPECT_EQ(stream.total_samples, 12000u);
        EXPECT_EQ(hash_of(stream), reference.integer_hash)
            << "actual hash: 0x" << std::hex << hash_of(stream);
    }
}

// The output line is what answers the open question about the tolerance: it shows the deviation
// that really occurs on each platform in the CI log.
TEST(Conformance, PcmOutputStaysWithinTheTolerance) {
    for (const Reference& reference : kReferences) {
        SCOPED_TRACE(reference.name);
        const std::string name = reference.name;
        const Pcm decoded = codec::decode(read_bytes(data_path(name + ".bin"))).samples;
        const Pcm expected = codec::read_wav(data_path(name + ".wav")).samples;
        ASSERT_EQ(decoded.size(), expected.size());

        int largest = 0;
        std::size_t differing = 0;
        for (std::size_t n = 0; n < decoded.size(); ++n) {
            const int deviation = std::abs(int{decoded[n]} - int{expected[n]});
            largest = deviation > largest ? deviation : largest;
            differing += deviation != 0 ? 1u : 0u;
        }
        std::cout << "[conformance] " << name << ": largest deviation " << largest << " LSB, "
                  << differing << " of " << decoded.size() << " samples differ\n";
        EXPECT_LE(largest, kToleranceLsb);
    }
}

TEST(Conformance, EncoderReachesTheQualityOfTheReference) {
    const codec::WavAudio input = codec::read_wav(data_path("input.wav"));
    for (const Reference& reference : kReferences) {
        SCOPED_TRACE(reference.name);
        const std::string name = reference.name;
        codec::EncoderOptions options;
        options.bitrate = reference.bitrate;
        options.psychoacoustics = reference.psychoacoustics;
        const Bytes stream = codec::encode(input, options);
        const Bytes reference_stream = read_bytes(data_path(name + ".bin"));

        const double snr = snr_db(input.samples, codec::decode(stream).samples);
        const double reference_snr = snr_db(input.samples, codec::decode(reference_stream).samples);
        std::cout << "[conformance] " << name << ": SNR " << snr << " dB, reference "
                  << reference_snr << " dB, encoder output "
                  << (stream == reference_stream ? "identical to" : "differs from")
                  << " the reference\n";
        EXPECT_NEAR(snr, reference_snr, kSnrToleranceDb);
    }
}
