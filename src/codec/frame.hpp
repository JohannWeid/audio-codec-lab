#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "codec/bands.hpp"
#include "codec/bitstream.hpp"
#include "codec/entropy.hpp"
#include "codec/types.hpp"

namespace codec {

/// New samples per frame, which is also the number of coefficients per frame.
inline constexpr std::size_t kFrameLength = 1024;
static_assert(kBandEdges.back() == kFrameLength);

using Scalefactors = std::array<int, kBandCount>;

/// The integers that one frame of the bitstream holds. Scalefactors of bands without any nonzero
/// coefficient are not transmitted, so a decoded frame holds the value from the frame before.
struct FrameData {
    int gain = 0;
    Scalefactors scalefactors{};
    std::vector<std::int32_t> quantized = std::vector<std::int32_t>(kFrameLength);
};

/// What encoder and decoder know from the frames so far: the adaptive probabilities and the frame
/// before. Both sides must go through the same frames in the same order. Copying it is cheap, which
/// lets the rate loop try out a frame without changing the state.
struct FrameCoderState {
    static constexpr std::size_t kCoefficientContexts = 14;
    static constexpr std::size_t kScalefactorContexts = 2;

    /// The index of the probability of the empty flag of a band.
    static constexpr std::size_t empty_flag_context(std::size_t band, bool empty_in_previous_frame,
                                                    bool previous_band_empty) noexcept {
        return band * 4 + (empty_in_previous_frame ? 2u : 0u) + (previous_band_empty ? 1u : 0u);
    }

    std::array<AdaptiveBit, kBandCount * 4> band_empty{};
    std::array<AdaptiveValue, kCoefficientContexts> coefficient{};
    std::array<AdaptiveValue, kScalefactorContexts> scalefactor{};
    AdaptiveValue gain_change{};

    int previous_gain = 144;
    Scalefactors previous_scalefactors{};
    std::array<int, kBandCount> previous_step_index{};
    std::array<bool, kBandCount> previous_band_empty{};
    std::array<std::uint8_t, kFrameLength> previous_bit_length{};
};

/// Quantizes kFrameLength coefficients with the step of each band. Throws std::invalid_argument if
/// the number of coefficients, the gain or a scalefactor is wrong.
FrameData quantize_frame(const std::vector<Sample>& coefficients, int gain,
                         const Scalefactors& scalefactors);

/// Inverse of quantize_frame(). Throws std::invalid_argument for an invalid frame.
std::vector<Sample> dequantize_frame(const FrameData& frame);

/// Codes one frame as described in section 3 of spec/codec-spec.md and moves the state on. Throws
/// std::invalid_argument for an invalid frame.
std::vector<std::uint8_t> encode_frame_payload(const FrameData& frame, FrameCoderState& state);

/// Decodes one frame and moves the state on. Throws BitstreamError if a value is out of range or
/// the data is not exactly what the encoder would have written at its end. A range coder cannot
/// detect every kind of damage, though, so changed data may also decode to different values.
FrameData decode_frame_payload(const std::uint8_t* data, std::size_t size, FrameCoderState& state);

}  // namespace codec
