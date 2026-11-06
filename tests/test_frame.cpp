#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "codec/bitstream.hpp"
#include "codec/entropy.hpp"
#include "codec/frame.hpp"
#include "codec/quantizer.hpp"
#include "test_helpers.hpp"

namespace {

using codec::best_rice_parameter;
using codec::BitReader;
using codec::BitstreamError;
using codec::BitWriter;
using codec::dequantize_frame;
using codec::FrameData;
using codec::kBandCount;
using codec::kBandEdges;
using codec::kFrameLength;
using codec::kMaxBandRiceParameter;
using codec::kMaxQuantized;
using codec::quantize_frame;
using codec::read_frame_payload;
using codec::Sample;
using codec::Scalefactors;
using codec::write_exp_golomb;
using codec::write_frame_payload;
using codec::write_rice;
using codec::zigzag_encode;

std::string bit_text(const std::vector<std::uint8_t>& bytes) {
    std::string text;
    for (const std::uint8_t byte : bytes) {
        for (int bit = 7; bit >= 0; --bit) {
            text += ((byte >> bit) & 1) != 0 ? '1' : '0';
        }
    }
    return text;
}

std::vector<std::uint8_t> payload_of(const FrameData& frame) {
    BitWriter writer;
    write_frame_payload(frame, writer);
    return writer.bytes();
}

FrameData roundtrip(const FrameData& frame) {
    const std::vector<std::uint8_t> bytes = payload_of(frame);
    BitReader reader(bytes.data(), bytes.size());
    FrameData back = read_frame_payload(reader);
    EXPECT_EQ(reader.bits_remaining(), 0u);
    return back;
}

// A band has only zeros if all of its coefficients are zero; the decoder then keeps the scalefactor
// of the band before it.
Scalefactors transmitted_scalefactors(const FrameData& frame) {
    Scalefactors expected{};
    int running = 0;
    for (std::size_t band = 0; band < kBandCount; ++band) {
        bool is_zero = true;
        for (std::size_t k = kBandEdges[band]; k < kBandEdges[band + 1]; ++k) {
            is_zero = is_zero && frame.quantized[k] == 0;
        }
        if (!is_zero) {
            running = frame.scalefactors[band];
        }
        expected[band] = running;
    }
    return expected;
}

// The start of a payload whose first band is not empty, up to where its scalefactor is written.
BitWriter start_of_nonzero_band() {
    BitWriter writer;
    writer.write_bits(0, 8);
    writer.write_bit(false);
    return writer;
}

// Completes a frame with zeros for the rest of the first band and 24 empty bands. Apart from what a
// test wrote before this, the frame is valid, so only the check under test can reject it. Random
// data behind the damage would not show this, since the reader would then run out of data anyway.
std::vector<std::uint8_t> finish_frame(BitWriter writer, int remaining_values, int k) {
    for (int i = 0; i < remaining_values; ++i) {
        write_rice(writer, 0, k);
    }
    writer.write_bits(0xFFFFFFu, 24);
    writer.align_to_byte();
    return writer.bytes();
}

constexpr int kFirstBandWidth = 4;

void expect_rejected(const std::vector<std::uint8_t>& bytes) {
    BitReader reader(bytes.data(), bytes.size());
    EXPECT_THROW(read_frame_payload(reader), BitstreamError);
}

}  // namespace

TEST(FramePayload, HasTheSpecifiedBitLayout) {
    FrameData frame;
    frame.gain = 0x12;
    frame.quantized[0] = 1;
    frame.quantized[3] = -1;
    // gain, band 0 is not empty, scalefactor difference 0, parameter difference 0, the four values
    // 1, 0, 0, -1 with k = 0, 24 empty bands, and the padding up to the byte boundary.
    const std::string expected =
        "00010010"
        "0"
        "1"
        "1"
        "110"
        "0"
        "0"
        "10" +
        std::string(24, '1') + "000000";
    EXPECT_EQ(bit_text(payload_of(frame)), expected);
}

TEST(FramePayload, EmptyFrameTakesFiveBytes) {
    FrameData frame;
    frame.gain = 255;
    const std::string expected = "11111111" + std::string(25, '1') + "0000000";
    EXPECT_EQ(bit_text(payload_of(frame)), expected);
}

TEST(FramePayload, RoundtripsVariedContent) {
    std::mt19937 rng(31);
    for (int round = 0; round < 200; ++round) {
        SCOPED_TRACE(round);
        FrameData frame;
        frame.gain = static_cast<int>(rng() % 256);
        for (std::size_t band = 0; band < kBandCount; ++band) {
            frame.scalefactors[band] = static_cast<int>(rng() % 256) - 128;
            const std::uint32_t kind = rng() % 4;
            if (kind == 0) {
                continue;  // An empty band.
            }
            const int bits = kind == 1 ? 2 : (kind == 2 ? 8 : 20);
            for (std::size_t k = kBandEdges[band]; k < kBandEdges[band + 1]; ++k) {
                const auto magnitude = static_cast<std::int32_t>(rng() % (1u << bits));
                frame.quantized[k] = rng() % 2 == 0 ? magnitude : -magnitude;
            }
        }
        const FrameData back = roundtrip(frame);
        EXPECT_EQ(back.gain, frame.gain);
        EXPECT_EQ(back.quantized, frame.quantized);
        EXPECT_EQ(back.scalefactors, transmitted_scalefactors(frame));
    }
}

TEST(FramePayload, RoundtripsTheLargestValues) {
    FrameData frame;
    frame.quantized[0] = kMaxQuantized;
    frame.quantized[1] = -kMaxQuantized;
    frame.quantized[1023] = kMaxQuantized;
    frame.scalefactors.fill(127);
    frame.gain = 255;
    const FrameData back = roundtrip(frame);
    EXPECT_EQ(back.quantized, frame.quantized);
    EXPECT_EQ(back.gain, 255);
}

TEST(FramePayload, WriterRejectsInvalidFrames) {
    BitWriter writer;

    FrameData wrong_size;
    wrong_size.quantized.pop_back();
    EXPECT_THROW(write_frame_payload(wrong_size, writer), std::invalid_argument);

    FrameData wrong_gain;
    wrong_gain.gain = 256;
    EXPECT_THROW(write_frame_payload(wrong_gain, writer), std::invalid_argument);

    FrameData wrong_scalefactor;
    wrong_scalefactor.scalefactors[3] = 128;
    EXPECT_THROW(write_frame_payload(wrong_scalefactor, writer), std::invalid_argument);

    FrameData too_large;
    too_large.quantized[5] = kMaxQuantized + 1;
    EXPECT_THROW(write_frame_payload(too_large, writer), std::invalid_argument);

    FrameData too_small;
    too_small.quantized[5] = -kMaxQuantized - 1;
    EXPECT_THROW(write_frame_payload(too_small, writer), std::invalid_argument);
}

TEST(FramePayload, ReaderRejectsScalefactorOutOfRange) {
    for (const int value : {128, -129, 100000}) {
        SCOPED_TRACE(value);
        BitWriter writer = start_of_nonzero_band();
        write_exp_golomb(writer, zigzag_encode(value));
        write_exp_golomb(writer, zigzag_encode(0));
        expect_rejected(finish_frame(std::move(writer), kFirstBandWidth, 0));
    }
}

TEST(FramePayload, ReaderRejectsRiceParameterOutOfRange) {
    for (const int value : {-1, kMaxBandRiceParameter + 1, 100000}) {
        SCOPED_TRACE(value);
        BitWriter writer = start_of_nonzero_band();
        write_exp_golomb(writer, zigzag_encode(0));
        write_exp_golomb(writer, zigzag_encode(value));
        const int k = value >= 0 && value <= codec::kMaxRiceParameter ? value : 0;
        expect_rejected(finish_frame(std::move(writer), kFirstBandWidth, k));
    }
}

TEST(FramePayload, ReaderRejectsValuesOutOfRange) {
    BitWriter writer = start_of_nonzero_band();
    write_exp_golomb(writer, zigzag_encode(0));
    write_exp_golomb(writer, zigzag_encode(0));
    write_rice(writer, zigzag_encode(kMaxQuantized) + 1, 0);
    expect_rejected(finish_frame(std::move(writer), kFirstBandWidth - 1, 0));
}

// Shows that the frames in the three tests above fail because of their one damaged field.
TEST(FramePayload, ReaderAcceptsTheSameFramesWithValidFields) {
    for (const int largest_scalefactor : {codec::kMaxScalefactor, codec::kMinScalefactor}) {
        BitWriter writer = start_of_nonzero_band();
        write_exp_golomb(writer, zigzag_encode(largest_scalefactor));
        write_exp_golomb(writer, zigzag_encode(kMaxBandRiceParameter));
        write_rice(writer, zigzag_encode(kMaxQuantized), kMaxBandRiceParameter);
        const std::vector<std::uint8_t> bytes =
            finish_frame(std::move(writer), kFirstBandWidth - 1, kMaxBandRiceParameter);

        BitReader reader(bytes.data(), bytes.size());
        const FrameData frame = read_frame_payload(reader);
        EXPECT_EQ(frame.quantized[0], kMaxQuantized);
        EXPECT_EQ(frame.scalefactors[0], largest_scalefactor);
        EXPECT_EQ(reader.bits_remaining(), 0u);
    }
}

TEST(FramePayload, ReaderRejectsPaddingThatIsNotZero) {
    BitWriter writer;
    writer.write_bits(0, 8);
    writer.write_bits(0x1FFFFFFu, 25);  // 25 empty bands.
    writer.write_bits(1, 7);            // The padding of the last byte.
    BitReader reader(writer.bytes().data(), writer.bytes().size());
    EXPECT_THROW(read_frame_payload(reader), BitstreamError);
}

TEST(FramePayload, ReaderRejectsEveryTruncation) {
    FrameData frame;
    frame.quantized[0] = 5;
    frame.quantized[700] = -9;
    const std::vector<std::uint8_t> bytes = payload_of(frame);
    for (std::size_t size = 0; size < bytes.size(); ++size) {
        SCOPED_TRACE(size);
        BitReader reader(bytes.data(), size);
        EXPECT_THROW(read_frame_payload(reader), BitstreamError);
    }
}

TEST(FramePayload, RandomDataIsHandled) {
    std::mt19937 rng(32);
    std::vector<std::uint8_t> bytes(2000);
    int accepted = 0;
    for (int round = 0; round < 3000; ++round) {
        for (std::uint8_t& byte : bytes) {
            byte = static_cast<std::uint8_t>(rng() & 0xFFu);
        }
        BitReader reader(bytes.data(), bytes.size());
        try {
            const FrameData frame = read_frame_payload(reader);
            EXPECT_NO_THROW(dequantize_frame(frame));
            ++accepted;
        } catch (const BitstreamError&) {
            // Rejecting is fine. Any other exception fails the test.
        }
    }
    RecordProperty("accepted", accepted);
}

// Because of the escape, the cost can fall again after it has risen. With many zeros and a few very
// large values, each step of k costs more at first, until the large values drop below the escape.
// A search that stops at the first rise would miss the true minimum here.
TEST(BestRiceParameter, FindsTheMinimumBehindARisingCost) {
    std::vector<std::int32_t> values(12, 0);
    std::fill(values.begin() + 8, values.end(), 1 << 20);
    const auto cost = [&values](int k) {
        std::uint64_t sum = 0;
        for (const std::int32_t value : values) {
            sum += static_cast<std::uint64_t>(codec::rice_bits(zigzag_encode(value), k));
        }
        return sum;
    };
    ASSERT_GT(cost(1), cost(0));  // The precondition: the cost rises right at the start.

    const int chosen = best_rice_parameter(values.data(), values.size());
    EXPECT_GT(chosen, 1);
    for (int k = 0; k <= kMaxBandRiceParameter; ++k) {
        EXPECT_LE(cost(chosen), cost(k));
    }
}

TEST(BestRiceParameter, ChoosesTheCheapestParameter) {
    std::mt19937 rng(33);
    for (int round = 0; round < 200; ++round) {
        std::vector<std::int32_t> values(1 + rng() % 200);
        const int bits = static_cast<int>(rng() % 25);
        for (std::int32_t& value : values) {
            const auto magnitude = static_cast<std::int32_t>(rng() % (std::uint32_t{1} << bits));
            value = rng() % 2 == 0 ? magnitude : -magnitude;
        }
        const auto cost = [&values](int k) {
            std::uint64_t sum = 0;
            for (const std::int32_t value : values) {
                sum += static_cast<std::uint64_t>(codec::rice_bits(zigzag_encode(value), k));
            }
            return sum;
        };
        const int chosen = best_rice_parameter(values.data(), values.size());
        ASSERT_GE(chosen, 0);
        ASSERT_LE(chosen, kMaxBandRiceParameter);
        for (int k = 0; k <= kMaxBandRiceParameter; ++k) {
            ASSERT_LE(cost(chosen), cost(k));
        }
    }
}

TEST(BestRiceParameter, HandlesSimpleCases) {
    const std::vector<std::int32_t> zeros(16, 0);
    EXPECT_EQ(best_rice_parameter(zeros.data(), zeros.size()), 0);
    EXPECT_EQ(best_rice_parameter(nullptr, 0), 0);

    const std::vector<std::int32_t> large(16, 1 << 20);
    EXPECT_GE(best_rice_parameter(large.data(), large.size()), 15);
}

TEST(QuantizeFrame, UsesTheStepOfEachBand) {
    std::vector<Sample> coefficients(kFrameLength, 1.0);
    Scalefactors scalefactors{};
    scalefactors[0] = 8;   // One octave finer than the gain.
    scalefactors[1] = -8;  // One octave coarser than the gain.
    const FrameData frame = quantize_frame(coefficients, 144 - 16, scalefactors);

    EXPECT_EQ(frame.quantized[kBandEdges[0]], 8);
    EXPECT_EQ(frame.quantized[kBandEdges[1]], 2);
    EXPECT_EQ(frame.quantized[kBandEdges[2]], 4);
    EXPECT_EQ(frame.quantized[kFrameLength - 1], 4);
}

TEST(QuantizeFrame, ReconstructionErrorStaysWithinHalfAStep) {
    std::mt19937 rng(34);
    const std::vector<Sample> coefficients = test::random_signal(kFrameLength, 35);
    Scalefactors scalefactors{};
    for (int& scalefactor : scalefactors) {
        scalefactor = static_cast<int>(rng() % 41) - 20;
    }
    const int gain = 100;
    const FrameData frame = quantize_frame(coefficients, gain, scalefactors);
    const std::vector<Sample> back = dequantize_frame(frame);

    for (std::size_t band = 0; band < kBandCount; ++band) {
        const Sample step = codec::step_size(gain, scalefactors[band]);
        for (std::size_t k = kBandEdges[band]; k < kBandEdges[band + 1]; ++k) {
            ASSERT_LE(std::fabs(coefficients[k] - back[k]), step / 2 * (1 + 1e-12));
        }
    }
}

TEST(QuantizeFrame, RejectsInvalidArguments) {
    const std::vector<Sample> right(kFrameLength);
    const std::vector<Sample> wrong(kFrameLength - 1);
    EXPECT_THROW(quantize_frame(wrong, 100, Scalefactors{}), std::invalid_argument);
    EXPECT_THROW(quantize_frame(right, 256, Scalefactors{}), std::invalid_argument);

    Scalefactors bad{};
    bad[24] = -129;
    EXPECT_THROW(quantize_frame(right, 100, bad), std::invalid_argument);

    FrameData frame;
    frame.quantized.pop_back();
    EXPECT_THROW(dequantize_frame(frame), std::invalid_argument);
}
