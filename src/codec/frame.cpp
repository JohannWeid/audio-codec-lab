#include "codec/frame.hpp"

#include <algorithm>
#include <stdexcept>

#include "codec/quantizer.hpp"

namespace codec {
namespace {

// A frame without any nonzero coefficient is rare in most signals, so its flag has a fixed
// probability. This also limits its cost to 4 bits, whatever came before, so an empty frame fits
// every budget.
constexpr std::uint32_t kProbabilityFrameNotEmpty = kProbabilityOne / 16 * 15;

void check_frame(const FrameData& frame) {
    if (frame.quantized.size() != kFrameLength) {
        throw std::invalid_argument("a frame must hold 1024 coefficients");
    }
    for (const std::int32_t value : frame.quantized) {
        if (value < -kMaxQuantized || value > kMaxQuantized) {
            throw std::invalid_argument("a quantized value is out of range");
        }
    }
    // Checks the ranges of the gain and of all scalefactors.
    for (const int scalefactor : frame.scalefactors) {
        (void)step_size(frame.gain, scalefactor);
    }
}

bool band_is_zero(const std::vector<std::int32_t>& quantized, std::size_t band) {
    for (std::size_t k = kBandEdges[band]; k < kBandEdges[band + 1]; ++k) {
        if (quantized[k] != 0) {
            return false;
        }
    }
    return true;
}

std::uint32_t magnitude(std::int32_t value) noexcept {
    return value >= 0 ? static_cast<std::uint32_t>(value)
                      : static_cast<std::uint32_t>(-(value + 1)) + 1u;
}

// The bit length that a coefficient of the frame before would have with the current step. Steps
// double every 8 units. Everything here is integer arithmetic, so that encoder and decoder agree on
// every platform.
int scaled_previous_length(const FrameCoderState& state, std::size_t k, int step_change) noexcept {
    const int length = state.previous_bit_length[k];
    return length == 0 ? 0 : std::max(0, length + step_change / 8);
}

// Chooses the probabilities for a coefficient by the size of its neighbors: the two coefficients
// below it in this frame and the three around it in the frame before.
std::size_t coefficient_context(const FrameCoderState& state,
                                const std::vector<std::int32_t>& quantized, std::size_t k,
                                int step_change) noexcept {
    const int below_1 = k >= 1 ? bit_length(magnitude(quantized[k - 1])) : 0;
    const int below_2 = k >= 2 ? bit_length(magnitude(quantized[k - 2])) : 0;
    const int before = scaled_previous_length(state, k, step_change);
    const int before_lower = k >= 1 ? scaled_previous_length(state, k - 1, step_change) : 0;
    const int before_upper =
        k + 1 < kFrameLength ? scaled_previous_length(state, k + 1, step_change) : 0;
    const int sum = 2 * below_1 + below_2 + 2 * before + before_lower + before_upper;

    // The classes get wider with the sum, since small neighbors tell the most.
    constexpr int kUpperEdges[FrameCoderState::kCoefficientContexts - 1] = {
        0, 1, 2, 3, 4, 6, 8, 11, 15, 20, 27, 36, 50};
    std::size_t context = 0;
    while (context + 1 < FrameCoderState::kCoefficientContexts && sum > kUpperEdges[context]) {
        ++context;
    }
    return context;
}

// Remembers what the next frame uses as its frame before.
void remember_frame(FrameCoderState& state, int gain, const std::vector<std::int32_t>& quantized,
                    const Scalefactors& scalefactors, const std::array<bool, kBandCount>& empty) {
    state.previous_gain = gain;
    state.previous_scalefactors = scalefactors;
    state.previous_band_empty = empty;
    for (std::size_t band = 0; band < kBandCount; ++band) {
        state.previous_step_index[band] = gain - scalefactors[band];
    }
    for (std::size_t k = 0; k < kFrameLength; ++k) {
        state.previous_bit_length[k] =
            static_cast<std::uint8_t>(bit_length(magnitude(quantized[k])));
    }
}

std::size_t scalefactor_context(bool last_change_was_zero) noexcept {
    return last_change_was_zero ? 0 : 1;
}

}  // namespace

FrameData quantize_frame(const std::vector<Sample>& coefficients, int gain,
                         const Scalefactors& scalefactors) {
    if (coefficients.size() != kFrameLength) {
        throw std::invalid_argument("a frame must hold 1024 coefficients");
    }
    FrameData frame;
    frame.gain = gain;
    frame.scalefactors = scalefactors;
    for (std::size_t band = 0; band < kBandCount; ++band) {
        const Sample step = step_size(gain, scalefactors[band]);
        for (std::size_t k = kBandEdges[band]; k < kBandEdges[band + 1]; ++k) {
            frame.quantized[k] = quantize(coefficients[k], step);
        }
    }
    return frame;
}

std::vector<Sample> dequantize_frame(const FrameData& frame) {
    check_frame(frame);
    std::vector<Sample> coefficients(kFrameLength);
    for (std::size_t band = 0; band < kBandCount; ++band) {
        const Sample step = step_size(frame.gain, frame.scalefactors[band]);
        for (std::size_t k = kBandEdges[band]; k < kBandEdges[band + 1]; ++k) {
            coefficients[k] = dequantize(frame.quantized[k], step);
        }
    }
    return coefficients;
}

std::vector<std::uint8_t> encode_frame_payload(const FrameData& frame, FrameCoderState& state) {
    check_frame(frame);
    RangeEncoder encoder;

    std::array<bool, kBandCount> empty{};
    bool frame_is_empty = true;
    for (std::size_t band = 0; band < kBandCount; ++band) {
        empty[band] = band_is_zero(frame.quantized, band);
        frame_is_empty = frame_is_empty && empty[band];
    }
    encoder.encode(kProbabilityFrameNotEmpty, frame_is_empty);
    if (frame_is_empty) {
        remember_frame(state, state.previous_gain, frame.quantized, state.previous_scalefactors,
                       empty);
        return encoder.finish();
    }

    encode_value(encoder, state.gain_change, zigzag_encode(frame.gain - state.previous_gain));

    Scalefactors transmitted = state.previous_scalefactors;
    bool previous_band_empty = false;
    bool last_change_was_zero = true;
    for (std::size_t band = 0; band < kBandCount; ++band) {
        const std::size_t flag = FrameCoderState::empty_flag_context(
            band, state.previous_band_empty[band], previous_band_empty);
        encoder.encode(state.band_empty[flag], empty[band]);
        previous_band_empty = empty[band];
        if (empty[band]) {
            continue;
        }

        const int change = frame.scalefactors[band] - state.previous_scalefactors[band];
        encode_value(encoder, state.scalefactor[scalefactor_context(last_change_was_zero)],
                     zigzag_encode(change));
        last_change_was_zero = change == 0;
        transmitted[band] = frame.scalefactors[band];

        const int step_change =
            state.previous_step_index[band] - (frame.gain - frame.scalefactors[band]);
        for (std::size_t k = kBandEdges[band]; k < kBandEdges[band + 1]; ++k) {
            const std::size_t context = coefficient_context(state, frame.quantized, k, step_change);
            encode_value(encoder, state.coefficient[context], magnitude(frame.quantized[k]));
            if (frame.quantized[k] != 0) {
                encoder.encode_direct(frame.quantized[k] < 0 ? 1u : 0u, 1);
            }
        }
    }
    remember_frame(state, frame.gain, frame.quantized, transmitted, empty);
    return encoder.finish();
}

FrameData decode_frame_payload(const std::uint8_t* data, std::size_t size, FrameCoderState& state) {
    // Trailing zeros are never written, so a payload that ends with one has been changed.
    if (size > 0 && data[size - 1] == 0) {
        throw BitstreamError("a frame ends with a zero byte");
    }
    RangeDecoder decoder(data, size);

    FrameData frame;
    frame.gain = state.previous_gain;
    frame.scalefactors = state.previous_scalefactors;

    std::array<bool, kBandCount> empty{};
    empty.fill(true);
    if (decoder.decode(kProbabilityFrameNotEmpty)) {
        if (!decoder.has_read_all_data()) {
            throw BitstreamError("a frame holds more data than its content needs");
        }
        remember_frame(state, frame.gain, frame.quantized, frame.scalefactors, empty);
        return frame;
    }

    const std::int64_t gain =
        std::int64_t{state.previous_gain} + zigzag_decode(decode_value(decoder, state.gain_change));
    if (gain < kMinGain || gain > kMaxGain) {
        throw BitstreamError("a gain is out of range");
    }
    frame.gain = static_cast<int>(gain);

    bool previous_band_empty = false;
    bool last_change_was_zero = true;
    const std::uint32_t largest_magnitude = static_cast<std::uint32_t>(kMaxQuantized);
    for (std::size_t band = 0; band < kBandCount; ++band) {
        const std::size_t flag = FrameCoderState::empty_flag_context(
            band, state.previous_band_empty[band], previous_band_empty);
        empty[band] = decoder.decode(state.band_empty[flag]);
        previous_band_empty = empty[band];
        if (empty[band]) {
            continue;
        }

        const std::int32_t change = zigzag_decode(
            decode_value(decoder, state.scalefactor[scalefactor_context(last_change_was_zero)]));
        const std::int64_t scalefactor =
            std::int64_t{state.previous_scalefactors[band]} + std::int64_t{change};
        if (scalefactor < kMinScalefactor || scalefactor > kMaxScalefactor) {
            throw BitstreamError("a scalefactor is out of range");
        }
        last_change_was_zero = change == 0;
        frame.scalefactors[band] = static_cast<int>(scalefactor);

        const int step_change =
            state.previous_step_index[band] - (frame.gain - frame.scalefactors[band]);
        for (std::size_t k = kBandEdges[band]; k < kBandEdges[band + 1]; ++k) {
            const std::size_t context = coefficient_context(state, frame.quantized, k, step_change);
            const std::uint32_t value = decode_value(decoder, state.coefficient[context]);
            if (value > largest_magnitude) {
                throw BitstreamError("a quantized value is out of range");
            }
            const auto signed_value = static_cast<std::int32_t>(value);
            frame.quantized[k] =
                value != 0 && decoder.decode_direct(1) != 0 ? -signed_value : signed_value;
        }
    }
    if (!decoder.has_read_all_data()) {
        throw BitstreamError("a frame holds more data than its content needs");
    }
    remember_frame(state, frame.gain, frame.quantized, frame.scalefactors, empty);
    return frame;
}

}  // namespace codec
