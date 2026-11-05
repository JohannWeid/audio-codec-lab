#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

#include "codec/codec.hpp"
#include "codec/version.hpp"
#include "codec/wav.hpp"

namespace {

constexpr int kExitError = 1;
constexpr int kExitUsage = 2;

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("cannot open " + path.u8string());
    }
    const std::vector<char> raw((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
    if (file.bad()) {
        throw std::runtime_error("cannot read " + path.u8string());
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(raw.size());
    for (const char c : raw) {
        bytes.push_back(static_cast<std::uint8_t>(static_cast<unsigned char>(c)));
    }
    return bytes;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "codec-decode " << codec::version() << "\n"
                  << "usage: codec-decode in.bin out.wav\n";
        return kExitUsage;
    }

    try {
        const codec::WavAudio audio = codec::decode(read_file(argv[1]));
        codec::write_wav(argv[2], audio);
        std::cout << argv[1] << ": " << audio.samples.size() << " samples written to " << argv[2]
                  << "\n";
    } catch (const std::exception& error) {
        std::cerr << "codec-decode: " << error.what() << "\n";
        return kExitError;
    }
    return 0;
}
