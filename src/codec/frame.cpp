#include "codec/frame.hpp"

#include <limits>
#include <stdexcept>

#include "codec/entropy.hpp"
#include "codec/quantizer.hpp"

namespace codec {
namespace {

constexpr int kGainBits = 8;

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

bool band_is_zero(const std::int32_t* values, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        if (values[i] != 0) {
            return false;
        }
    }
    return true;
}

// Reads a difference and adds it to the previous value. Computed with a wider type, because a
// damaged code can hold any 32-bit value.
int read_delta(BitReader& reader, int previous, int minimum, int maximum, const char* message) {
    const std::int64_t value =
        std::int64_t{previous} + std::int64_t{zigzag_decode(read_exp_golomb(reader))};
    if (value < minimum || value > maximum) {
        throw BitstreamError(message);
    }
    return static_cast<int>(value);
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

int best_rice_parameter(const std::int32_t* values, std::size_t count) {
    int best = 0;
    std::uint64_t best_bits = std::numeric_limits<std::uint64_t>::max();
    for (int k = 0; k <= kMaxBandRiceParameter; ++k) {
        std::uint64_t bits = 0;
        for (std::size_t i = 0; i < count; ++i) {
            bits += static_cast<std::uint64_t>(rice_bits(zigzag_encode(values[i]), k));
        }
        // All parameters must be tried: because of the escape, the cost can have several minima.
        if (bits < best_bits) {
            best_bits = bits;
            best = k;
        }
    }
    return best;
}

void write_frame_payload(const FrameData& frame, BitWriter& writer) {
    check_frame(frame);
    writer.write_bits(static_cast<std::uint32_t>(frame.gain), kGainBits);

    int previous_scalefactor = 0;
    int previous_parameter = 0;
    for (std::size_t band = 0; band < kBandCount; ++band) {
        const std::int32_t* values = frame.quantized.data() + kBandEdges[band];
        const std::size_t count = kBandEdges[band + 1] - kBandEdges[band];
        const bool is_zero = band_is_zero(values, count);
        writer.write_bit(is_zero);
        if (is_zero) {
            continue;
        }

        const int scalefactor = frame.scalefactors[band];
        write_exp_golomb(writer, zigzag_encode(scalefactor - previous_scalefactor));
        previous_scalefactor = scalefactor;

        const int parameter = best_rice_parameter(values, count);
        write_exp_golomb(writer, zigzag_encode(parameter - previous_parameter));
        previous_parameter = parameter;

        for (std::size_t i = 0; i < count; ++i) {
            write_rice(writer, zigzag_encode(values[i]), parameter);
        }
    }
    writer.align_to_byte();
}

FrameData read_frame_payload(BitReader& reader) {
    const std::uint32_t largest_code = zigzag_encode(kMaxQuantized);

    FrameData frame;
    frame.gain = static_cast<int>(reader.read_bits(kGainBits));

    int scalefactor = 0;
    int parameter = 0;
    for (std::size_t band = 0; band < kBandCount; ++band) {
        const bool is_zero = reader.read_bit();
        if (!is_zero) {
            scalefactor = read_delta(reader, scalefactor, kMinScalefactor, kMaxScalefactor,
                                     "a scalefactor is out of range");
            parameter = read_delta(reader, parameter, 0, kMaxBandRiceParameter,
                                   "a Rice parameter is out of range");
            for (std::size_t k = kBandEdges[band]; k < kBandEdges[band + 1]; ++k) {
                const std::uint32_t code = read_rice(reader, parameter);
                if (code > largest_code) {
                    throw BitstreamError("a quantized value is out of range");
                }
                frame.quantized[k] = zigzag_decode(code);
            }
        }
        frame.scalefactors[band] = scalefactor;
    }
    reader.skip_padding();
    return frame;
}

}  // namespace codec
