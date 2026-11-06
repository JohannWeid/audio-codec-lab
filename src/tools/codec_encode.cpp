#include <charconv>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "codec/codec.hpp"
#include "codec/version.hpp"
#include "codec/wav.hpp"

namespace {

constexpr int kExitError = 1;
constexpr int kExitUsage = 2;

int usage() {
    std::cerr << "codec-encode " << codec::version() << "\n"
              << "usage: codec-encode in.wav out.bin --bitrate 64000 [--psy on|off]\n"
              << "The input must be 16-bit PCM, 48 kHz, mono. The bitrate is in bit/s, from "
              << codec::kMinBitrate << " to " << codec::kMaxBitrate << ".\n"
              << "--psy off selects the baseline with flat noise instead of the masking model.\n";
    return kExitUsage;
}

bool parse_bitrate(const std::string& text, std::uint32_t& bitrate) {
    const char* const end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, bitrate);
    return result.ec == std::errc{} && result.ptr == end;
}

void write_file(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        throw std::runtime_error("cannot create " + path.u8string());
    }
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    file.close();
    if (file.fail()) {
        throw std::runtime_error("cannot write " + path.u8string());
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> positional;
    std::uint32_t bitrate = 0;
    bool have_bitrate = false;
    bool psychoacoustics = true;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--bitrate") {
            if (i + 1 == argc || !parse_bitrate(argv[i + 1], bitrate)) {
                return usage();
            }
            have_bitrate = true;
            ++i;
        } else if (argument == "--psy") {
            const std::string value = i + 1 < argc ? argv[i + 1] : "";
            if (value != "on" && value != "off") {
                return usage();
            }
            psychoacoustics = value == "on";
            ++i;
        } else {
            positional.push_back(argument);
        }
    }
    if (positional.size() != 2 || !have_bitrate) {
        return usage();
    }

    try {
        const codec::WavAudio audio = codec::read_wav(positional[0]);
        codec::EncoderOptions options;
        options.bitrate = bitrate;
        options.psychoacoustics = psychoacoustics;
        const std::vector<std::uint8_t> stream = codec::encode(audio, options);
        write_file(positional[1], stream);

        std::cout << positional[0] << ": " << audio.samples.size() << " samples, " << stream.size()
                  << " bytes written to " << positional[1] << "\n";
    } catch (const std::exception& error) {
        std::cerr << "codec-encode: " << error.what() << "\n";
        return kExitError;
    }
    return 0;
}
