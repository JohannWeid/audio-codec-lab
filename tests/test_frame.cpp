#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <utility>
#include <vector>

#include "codec/entropy.hpp"
#include "codec/frame.hpp"
#include "codec/quantizer.hpp"
#include "test_helpers.hpp"

namespace {

using Bytes = std::vector<std::uint8_t>;
using codec::BitstreamError;
using codec::decode_frame_payload;
using codec::dequantize_frame;
using codec::encode_frame_payload;
using codec::encode_value;
using codec::FrameCoderState;
using codec::FrameData;
using codec::kBandCount;
using codec::kBandEdges;
using codec::kFrameLength;
using codec::kMaxQuantized;
using codec::kProbabilityOne;
using codec::quantize_frame;
using codec::RangeEncoder;
using codec::Sample;
using codec::Scalefactors;
using codec::zigzag_encode;

// A frame with random content, in which some of the bands stay empty.
FrameData random_frame(std::mt19937& rng) {
    FrameData frame;
    frame.gain = static_cast<int>(rng() % 256);
    for (std::size_t band = 0; band < kBandCount; ++band) {
        frame.scalefactors[band] = static_cast<int>(rng() % 256) - 128;
        const std::uint32_t kind = rng() % 4;
        if (kind == 0) {
            continue;
        }
        const int bits = kind == 1 ? 2 : (kind == 2 ? 8 : 20);
        for (std::size_t k = kBandEdges[band]; k < kBandEdges[band + 1]; ++k) {
            const auto magnitude = static_cast<std::int32_t>(rng() % (1u << bits));
            frame.quantized[k] = rng() % 2 == 0 ? magnitude : -magnitude;
        }
    }
    return frame;
}

// Scalefactors of empty bands are not transmitted, so they keep the value of the frame before.
Scalefactors transmitted_scalefactors(const FrameData& frame, const Scalefactors& before) {
    Scalefactors expected = before;
    for (std::size_t band = 0; band < kBandCount; ++band) {
        for (std::size_t k = kBandEdges[band]; k < kBandEdges[band + 1]; ++k) {
            if (frame.quantized[k] != 0) {
                expected[band] = frame.scalefactors[band];
                break;
            }
        }
    }
    return expected;
}

// The start of a frame that is not empty, with this gain and a first band that is not empty either.
// It follows the layout in the specification, so that tests can build damaged frames from it.
RangeEncoder start_frame(FrameCoderState& state, int gain_change) {
    RangeEncoder encoder;
    encoder.encode(kProbabilityOne / 16 * 15, false);
    encode_value(encoder, state.gain_change, zigzag_encode(gain_change));
    encoder.encode(state.band_empty[FrameCoderState::empty_flag_context(0, false, false)], false);
    return encoder;
}

void expect_rejected(RangeEncoder encoder) {
    const Bytes bytes = encoder.finish();
    FrameCoderState state;
    EXPECT_THROW(decode_frame_payload(bytes.data(), bytes.size(), state), BitstreamError);
}

}  // namespace

TEST(FramePayload, RoundtripsASequenceOfFrames) {
    std::mt19937 rng(91);
    FrameCoderState encoder_state;
    FrameCoderState decoder_state;
    Scalefactors expected_scalefactors{};
    for (int round = 0; round < 100; ++round) {
        SCOPED_TRACE(round);
        const FrameData frame = random_frame(rng);
        const Bytes bytes = encode_frame_payload(frame, encoder_state);
        const FrameData back = decode_frame_payload(bytes.data(), bytes.size(), decoder_state);
        ASSERT_EQ(back.gain, frame.gain);
        ASSERT_EQ(back.quantized, frame.quantized);
        expected_scalefactors = transmitted_scalefactors(frame, expected_scalefactors);
        ASSERT_EQ(back.scalefactors, expected_scalefactors);
    }
}

TEST(FramePayload, RoundtripsTheLargestValues) {
    FrameData frame;
    frame.quantized[0] = kMaxQuantized;
    frame.quantized[1] = -kMaxQuantized;
    frame.quantized[1023] = kMaxQuantized;
    frame.scalefactors.fill(127);
    frame.gain = 255;
    FrameCoderState encoder_state;
    const Bytes bytes = encode_frame_payload(frame, encoder_state);
    FrameCoderState decoder_state;
    const FrameData back = decode_frame_payload(bytes.data(), bytes.size(), decoder_state);
    EXPECT_EQ(back.quantized, frame.quantized);
    EXPECT_EQ(back.gain, 255);
}

// The cost of an empty frame is limited, however much the probabilities have adapted, so that an
// empty frame fits the budget of the lowest bitrate.
TEST(FramePayload, EmptyFrameIsAlwaysSmall) {
    std::mt19937 rng(92);
    FrameCoderState state;
    EXPECT_LE(encode_frame_payload(FrameData{}, state).size(), 2u);
    for (int round = 0; round < 50; ++round) {
        (void)encode_frame_payload(random_frame(rng), state);
    }
    EXPECT_LE(encode_frame_payload(FrameData{}, state).size(), 2u);
}

TEST(FramePayload, EmptyFrameKeepsGainAndScalefactors) {
    FrameData first;
    first.gain = 77;
    first.scalefactors.fill(-5);
    first.quantized[10] = 3;
    FrameCoderState encoder_state;
    FrameCoderState decoder_state;
    const Bytes first_bytes = encode_frame_payload(first, encoder_state);
    (void)decode_frame_payload(first_bytes.data(), first_bytes.size(), decoder_state);

    FrameData empty;
    empty.gain = 200;  // Not transmitted, since nothing depends on it.
    const Bytes empty_bytes = encode_frame_payload(empty, encoder_state);
    const FrameData back =
        decode_frame_payload(empty_bytes.data(), empty_bytes.size(), decoder_state);
    EXPECT_EQ(back.gain, 77);
    EXPECT_EQ(back.scalefactors[0], 0);  // Band 0 was empty, so nothing changed it.
    EXPECT_EQ(back.scalefactors[2], -5);
    EXPECT_EQ(encoder_state.previous_gain, decoder_state.previous_gain);
    EXPECT_EQ(encoder_state.previous_scalefactors, decoder_state.previous_scalefactors);
}

// The probabilities and the frame before make a repeated frame cheaper.
TEST(FramePayload, RepeatedFramesGetCheaper) {
    std::mt19937 rng(93);
    FrameData frame;
    frame.gain = 150;
    for (std::size_t k = 0; k < kFrameLength; k += 5) {
        frame.quantized[k] = static_cast<std::int32_t>(rng() % 7) - 3;
    }
    FrameCoderState state;
    const std::size_t first = encode_frame_payload(frame, state).size();
    std::size_t later = 0;
    for (int round = 0; round < 20; ++round) {
        later = encode_frame_payload(frame, state).size();
    }
    EXPECT_LT(later, first);
}

// This was the limit of the Rice code: at least one bit per coefficient in every band that is not
// empty, which is 1024 bits for a frame with one value in each band.
TEST(FramePayload, SparseFramesCostFarLessThanOneBitPerCoefficient) {
    std::mt19937 rng(94);
    FrameCoderState state;
    std::size_t last = 0;
    for (int round = 0; round < 30; ++round) {
        FrameData frame;
        frame.gain = 150;
        for (std::size_t band = 0; band < kBandCount; ++band) {
            const std::size_t width = kBandEdges[band + 1] - kBandEdges[band];
            frame.quantized[kBandEdges[band] + rng() % width] = rng() % 2 == 0 ? 1 : -1;
        }
        last = encode_frame_payload(frame, state).size();
    }
    EXPECT_LT(8 * last, 400u);
}

TEST(FramePayload, WriterRejectsInvalidFrames) {
    FrameCoderState state;

    FrameData wrong_size;
    wrong_size.quantized.pop_back();
    EXPECT_THROW(encode_frame_payload(wrong_size, state), std::invalid_argument);

    FrameData wrong_gain;
    wrong_gain.gain = 256;
    EXPECT_THROW(encode_frame_payload(wrong_gain, state), std::invalid_argument);

    FrameData wrong_scalefactor;
    wrong_scalefactor.scalefactors[3] = 128;
    EXPECT_THROW(encode_frame_payload(wrong_scalefactor, state), std::invalid_argument);

    FrameData too_large;
    too_large.quantized[5] = kMaxQuantized + 1;
    EXPECT_THROW(encode_frame_payload(too_large, state), std::invalid_argument);

    FrameData too_small;
    too_small.quantized[5] = -kMaxQuantized - 1;
    EXPECT_THROW(encode_frame_payload(too_small, state), std::invalid_argument);
}

TEST(FramePayload, ReaderRejectsGainOutOfRange) {
    // The gain before the first frame is 144.
    for (const int change : {112, -145, 1000000}) {
        SCOPED_TRACE(change);
        FrameCoderState state;
        RangeEncoder encoder;
        encoder.encode(kProbabilityOne / 16 * 15, false);
        encode_value(encoder, state.gain_change, zigzag_encode(change));
        expect_rejected(std::move(encoder));
    }
}

TEST(FramePayload, ReaderRejectsScalefactorOutOfRange) {
    for (const int change : {128, -129, 1000000}) {
        SCOPED_TRACE(change);
        FrameCoderState state;
        RangeEncoder encoder = start_frame(state, 0);
        encode_value(encoder, state.scalefactor[0], zigzag_encode(change));
        expect_rejected(std::move(encoder));
    }
}

TEST(FramePayload, ReaderRejectsValuesOutOfRange) {
    FrameCoderState state;
    RangeEncoder encoder = start_frame(state, 0);
    encode_value(encoder, state.scalefactor[0], zigzag_encode(0));
    encode_value(encoder, state.coefficient[0], static_cast<std::uint32_t>(kMaxQuantized) + 1);
    expect_rejected(std::move(encoder));
}

// Shows that the frames in the tests above fail because of their one damaged field. The largest
// valid values at the same places are accepted and come back.
TEST(FramePayload, ReaderAcceptsTheLargestValidFields) {
    for (const int scalefactor : {codec::kMaxScalefactor, codec::kMinScalefactor}) {
        SCOPED_TRACE(scalefactor);
        for (const int gain : {codec::kMinGain, codec::kMaxGain}) {
            FrameData frame;
            frame.gain = gain;
            frame.scalefactors[0] = scalefactor;
            frame.quantized[0] = kMaxQuantized;
            FrameCoderState encoder_state;
            const Bytes bytes = encode_frame_payload(frame, encoder_state);
            FrameCoderState decoder_state;
            const FrameData back = decode_frame_payload(bytes.data(), bytes.size(), decoder_state);
            EXPECT_EQ(back.gain, gain);
            EXPECT_EQ(back.scalefactors[0], scalefactor);
            EXPECT_EQ(back.quantized[0], kMaxQuantized);
        }
    }
}

TEST(FramePayload, ReaderRejectsAPayloadThatEndsWithZero) {
    FrameData frame;
    frame.quantized[0] = 5;
    FrameCoderState encoder_state;
    Bytes bytes = encode_frame_payload(frame, encoder_state);
    ASSERT_NE(bytes.back(), 0);
    bytes.push_back(0);
    FrameCoderState decoder_state;
    EXPECT_THROW(decode_frame_payload(bytes.data(), bytes.size(), decoder_state), BitstreamError);
}

TEST(FramePayload, ReaderRejectsMoreDataThanTheContentNeeds) {
    FrameData frame;
    frame.quantized[0] = 5;
    FrameCoderState encoder_state;
    Bytes bytes = encode_frame_payload(frame, encoder_state);
    bytes.insert(bytes.end(), 40, 0xAB);
    FrameCoderState decoder_state;
    EXPECT_THROW(decode_frame_payload(bytes.data(), bytes.size(), decoder_state), BitstreamError);

    // The same for an empty frame, which takes the short way through the decoder.
    Bytes empty(40, 0xFF);
    FrameCoderState empty_state;
    EXPECT_THROW(decode_frame_payload(empty.data(), empty.size(), empty_state), BitstreamError);
}

// The sanitizer job in CI turns any read out of bounds or undefined behavior into a failure.
TEST(FramePayload, RandomDataIsHandled) {
    std::mt19937 rng(95);
    Bytes bytes(600);
    for (int round = 0; round < 3000; ++round) {
        for (std::uint8_t& byte : bytes) {
            byte = static_cast<std::uint8_t>(rng() & 0xFFu);
        }
        FrameCoderState state;
        try {
            const FrameData frame = decode_frame_payload(bytes.data(), bytes.size(), state);
            EXPECT_NO_THROW(dequantize_frame(frame));
        } catch (const BitstreamError&) {
            // Rejecting is fine. Any other exception fails the test.
        }
    }
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
    std::mt19937 rng(96);
    const std::vector<Sample> coefficients = test::random_signal(kFrameLength, 97);
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
