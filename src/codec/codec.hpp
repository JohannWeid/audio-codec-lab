#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "codec/allocation.hpp"
#include "codec/frame.hpp"
#include "codec/psychoacoustics.hpp"
#include "codec/wav.hpp"

namespace codec {

inline constexpr std::uint32_t kSampleRate = 48000;
inline constexpr std::uint32_t kMinBitrate = 8000;
inline constexpr std::uint32_t kMaxBitrate = 512000;

struct EncoderOptions {
    std::uint32_t bitrate = 64000;
    /// Off selects the flat noise allocation of the baseline, see section 7 of spec/codec-spec.md.
    bool psychoacoustics = true;
    /// The bandwidth limit of the model, in eighths of a bit per coded coefficient. Only for
    /// experiments: the default is the value that section 6 of spec/codec-spec.md explains, and the
    /// baseline has no limit at all.
    std::uint32_t bandwidth_limit_eighths = kDefaultBandwidthLimitEighths;
};

/// Number of bits that frame frame_index may use, including its length field. The sum over the
/// frames follows the bitrate exactly, see section 6 of spec/codec-spec.md.
std::uint32_t frame_budget_bits(std::uint64_t frame_index, std::uint32_t bitrate) noexcept;

/// Encodes 48 kHz mono audio. Throws std::invalid_argument for other audio, for a bitrate outside
/// of kMinBitrate to kMaxBitrate, or for a bandwidth limit outside of its range.
std::vector<std::uint8_t> encode(const WavAudio& audio, const EncoderOptions& options);

/// What happened in one frame of the encoder. All energies are on the scale of the MDCT
/// coefficients of the frame. The thresholds are there for flat noise as well, so that both can be
/// compared.
struct FrameReport {
    BandValues band_energy{};
    BandValues masking_threshold{};
    BandValues quiet_threshold{};
    BandValues noise{};
    std::size_t payload_bytes = 0;
    std::uint32_t budget_bits = 0;
};

/// Runs the same encoder as encode() and reports on each frame instead of returning the bitstream.
/// For the evaluation only, since it runs the model a second time.
std::vector<FrameReport> report_encoding(const WavAudio& audio, const EncoderOptions& options);

/// The content of a bitstream before the signal is reconstructed. These integers must be identical
/// on every platform, see section 4 of spec/codec-spec.md.
struct ParsedStream {
    std::uint64_t total_samples = 0;
    std::vector<FrameData> frames;
};

/// Reads a whole bitstream. Throws BitstreamError if it is damaged in any way.
ParsedStream parse_stream(const std::vector<std::uint8_t>& bytes);

/// Reconstructs the audio from a bitstream. Throws BitstreamError if it is damaged in any way.
WavAudio decode(const std::vector<std::uint8_t>& bytes);

}  // namespace codec
