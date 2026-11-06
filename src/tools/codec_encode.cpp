#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "codec/codec.hpp"
#include "codec/version.hpp"
#include "codec/wav.hpp"
#include "encoder_arguments.hpp"

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
    tools::EncoderArguments arguments;
    if (!tools::parse_encoder_arguments(argc, argv, arguments)) {
        return usage();
    }

    try {
        const codec::WavAudio audio = codec::read_wav(arguments.files[0]);
        const std::vector<std::uint8_t> stream = codec::encode(audio, arguments.options);
        write_file(arguments.files[1], stream);

        std::cout << arguments.files[0] << ": " << audio.samples.size() << " samples, "
                  << stream.size() << " bytes written to " << arguments.files[1] << "\n";
    } catch (const std::exception& error) {
        std::cerr << "codec-encode: " << error.what() << "\n";
        return kExitError;
    }
    return 0;
}
