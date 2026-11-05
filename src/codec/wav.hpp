#pragma once

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <vector>

namespace codec {

/// Thrown when a WAV file cannot be read, parsed or written.
class WavError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// 16-bit PCM audio. Samples of several channels are interleaved.
struct WavAudio {
    std::uint32_t sample_rate = 0;
    std::uint16_t channels = 0;
    std::vector<std::int16_t> samples;
};

/// Parses a WAV file held in memory. Only 16-bit PCM is accepted, anything else throws WavError.
/// Sample rate and channel count are not restricted here; the encoder enforces the limits of the
/// specification.
WavAudio parse_wav(const std::vector<std::uint8_t>& bytes);

/// Creates a WAV file in memory with a canonical 44-byte header. Throws WavError if the audio is
/// invalid or too large for the format.
std::vector<std::uint8_t> serialize_wav(const WavAudio& audio);

WavAudio read_wav(const std::filesystem::path& path);
void write_wav(const std::filesystem::path& path, const WavAudio& audio);

}  // namespace codec
