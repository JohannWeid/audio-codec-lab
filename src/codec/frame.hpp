#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "codec/bands.hpp"
#include "codec/bitstream.hpp"
#include "codec/types.hpp"

namespace codec {

/// New samples per frame, which is also the number of coefficients per frame.
inline constexpr std::size_t kFrameLength = 1024;
static_assert(kBandEdges.back() == kFrameLength);

/// Largest Rice parameter that a band may use.
inline constexpr int kMaxBandRiceParameter = 24;

using Scalefactors = std::array<int, kBandCount>;

/// The integers that one frame of the bitstream holds. Scalefactors of bands without any nonzero
/// coefficient are not transmitted, so a decoded frame holds the value of the band before them.
struct FrameData {
    int gain = 0;
    Scalefactors scalefactors{};
    std::vector<std::int32_t> quantized = std::vector<std::int32_t>(kFrameLength);
};

/// Quantizes kFrameLength coefficients with the step of each band. Throws std::invalid_argument if
/// the number of coefficients, the gain or a scalefactor is wrong.
FrameData quantize_frame(const std::vector<Sample>& coefficients, int gain,
                         const Scalefactors& scalefactors);

/// Inverse of quantize_frame(). Throws std::invalid_argument for an invalid frame.
std::vector<Sample> dequantize_frame(const FrameData& frame);

/// The Rice parameter from 0 to kMaxBandRiceParameter that gives the fewest bits for these values.
/// The smallest one wins a tie.
int best_rice_parameter(const std::int32_t* values, std::size_t count);

/// Writes one frame as described in section 3 of spec/codec-spec.md, ending at a byte boundary.
/// Throws std::invalid_argument for an invalid frame.
void write_frame_payload(const FrameData& frame, BitWriter& writer);

/// Reads one frame including the padding at its end. Throws BitstreamError if the data is damaged.
FrameData read_frame_payload(BitReader& reader);

}  // namespace codec
