#include "codec/codec.hpp"

#include <cstddef>
#include <stdexcept>

#include "codec/allocation.hpp"
#include "codec/framing.hpp"
#include "codec/mdct.hpp"
#include "codec/pcm.hpp"
#include "codec/psychoacoustics.hpp"

namespace codec {
namespace {

constexpr std::uint32_t kMagic = 0x41434C42;  // "ACLB"
constexpr std::uint32_t kVersion = 1;
constexpr std::uint32_t kChannels = 1;
constexpr std::size_t kHeaderSize = 20;
constexpr std::size_t kLengthFieldBytes = 2;
constexpr std::uint32_t kLengthFieldBits = 16;

// An empty frame still has a length field, a gain and one bit for each band, so 7 bytes in total.
constexpr std::size_t kSmallestFrameBytes = kLengthFieldBytes + 5;

}  // namespace

std::uint32_t frame_budget_bits(std::uint64_t frame_index, std::uint32_t bitrate) noexcept {
    const std::uint64_t scaled_rate = std::uint64_t{bitrate} * kFrameLength;
    const std::uint64_t end = (frame_index + 1) * scaled_rate / kSampleRate;
    const std::uint64_t start = frame_index * scaled_rate / kSampleRate;
    return static_cast<std::uint32_t>(end - start);
}

std::vector<std::uint8_t> encode(const WavAudio& audio, const EncoderOptions& options) {
    if (audio.sample_rate != kSampleRate) {
        throw std::invalid_argument("only 48 kHz audio is supported");
    }
    if (audio.channels != kChannels) {
        throw std::invalid_argument("only mono audio is supported");
    }
    if (options.bitrate < kMinBitrate || options.bitrate > kMaxBitrate) {
        throw std::invalid_argument("the bitrate must be in 8000 to 512000 bit/s");
    }

    const std::uint64_t total_samples = audio.samples.size();

    BitWriter header;
    header.write_bits(kMagic, 32);
    header.write_bits(kVersion, 8);
    header.write_bits(kSampleRate, 32);
    header.write_bits(kChannels, 8);
    header.write_bits(static_cast<std::uint32_t>(kFrameLength), 16);
    header.write_bits(static_cast<std::uint32_t>(total_samples >> 32), 32);
    header.write_bits(static_cast<std::uint32_t>(total_samples & 0xFFFFFFFFu), 32);

    std::vector<std::uint8_t> stream = header.bytes();

    const Mdct mdct(kFrameLength);
    const PsychoacousticModel model;
    const std::vector<std::vector<Sample>> blocks =
        split_into_blocks(pcm_to_samples(audio.samples), kFrameLength);
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        const std::uint32_t budget = frame_budget_bits(i, options.bitrate);
        const std::size_t max_payload_bytes = (budget - kLengthFieldBits) / 8;
        const std::vector<Sample> coefficients = mdct.forward(blocks[i]);
        const EncodedFrame encoded =
            options.psychoacoustics
                ? encode_perceptual(coefficients, model.analyze(blocks[i]), max_payload_bytes)
                : encode_flat(coefficients, max_payload_bytes);
        const std::vector<std::uint8_t>& payload = encoded.payload;
        stream.push_back(static_cast<std::uint8_t>(payload.size() >> 8));
        stream.push_back(static_cast<std::uint8_t>(payload.size() & 0xFFu));
        stream.insert(stream.end(), payload.begin(), payload.end());
    }
    return stream;
}

ParsedStream parse_stream(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < kHeaderSize) {
        throw BitstreamError("the file is too short for a header");
    }
    BitReader header(bytes.data(), kHeaderSize);
    if (header.read_bits(32) != kMagic) {
        throw BitstreamError("this is not a bitstream of this codec");
    }
    if (header.read_bits(8) != kVersion) {
        throw BitstreamError("the bitstream has an unknown version");
    }
    if (header.read_bits(32) != kSampleRate) {
        throw BitstreamError("the bitstream has an unsupported sample rate");
    }
    if (header.read_bits(8) != kChannels) {
        throw BitstreamError("the bitstream has an unsupported channel count");
    }
    if (header.read_bits(16) != kFrameLength) {
        throw BitstreamError("the bitstream has an unsupported frame length");
    }
    const std::uint64_t high = header.read_bits(32);
    const std::uint64_t low = header.read_bits(32);
    const std::uint64_t total_samples = (high << 32) | low;

    // Checked before computing or allocating anything from a value that may be damaged.
    const std::size_t remaining_bytes = bytes.size() - kHeaderSize;
    const std::uint64_t full_frames = total_samples / kFrameLength;
    if (full_frames > remaining_bytes / kSmallestFrameBytes) {
        throw BitstreamError("the file is too short for the announced number of samples");
    }
    const std::size_t count = frame_count(static_cast<std::size_t>(total_samples), kFrameLength);

    ParsedStream stream;
    stream.total_samples = total_samples;
    stream.frames.reserve(count);

    std::size_t position = kHeaderSize;
    for (std::size_t i = 0; i < count; ++i) {
        if (bytes.size() - position < kLengthFieldBytes) {
            throw BitstreamError("the bitstream ends inside a frame length");
        }
        const std::size_t payload_size =
            (std::size_t{bytes[position]} << 8) | std::size_t{bytes[position + 1]};
        position += kLengthFieldBytes;
        if (payload_size > bytes.size() - position) {
            throw BitstreamError("a frame extends beyond the end of the bitstream");
        }
        BitReader reader(bytes.data() + position, payload_size);
        stream.frames.push_back(read_frame_payload(reader));
        if (reader.bits_remaining() != 0) {
            throw BitstreamError("a frame holds more data than its content needs");
        }
        position += payload_size;
    }
    if (position != bytes.size()) {
        throw BitstreamError("there is data after the last frame");
    }
    return stream;
}

WavAudio decode(const std::vector<std::uint8_t>& bytes) {
    const ParsedStream stream = parse_stream(bytes);

    std::vector<std::vector<Sample>> coefficients;
    coefficients.reserve(stream.frames.size());
    for (const FrameData& frame : stream.frames) {
        coefficients.push_back(dequantize_frame(frame));
    }

    const Mdct mdct(kFrameLength);
    WavAudio audio;
    audio.sample_rate = kSampleRate;
    audio.channels = kChannels;
    audio.samples = samples_to_pcm(
        synthesize(coefficients, static_cast<std::size_t>(stream.total_samples), mdct));
    return audio;
}

}  // namespace codec
