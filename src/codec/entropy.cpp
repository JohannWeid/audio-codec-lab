#include "codec/entropy.hpp"

namespace codec {
namespace {

constexpr int kMaxBitLength = 32;

std::size_t model_index(int position) noexcept {
    const auto index = static_cast<std::size_t>(position);
    return index < AdaptiveValue::kModels ? index : AdaptiveValue::kModels - 1;
}

}  // namespace

std::uint32_t zigzag_encode(std::int32_t value) noexcept {
    // Written out, because shifting negative values is not fully portable in C++17. The negative
    // branch works for the smallest value too, since value + 1 cannot overflow there.
    return value >= 0 ? 2u * static_cast<std::uint32_t>(value)
                      : 2u * static_cast<std::uint32_t>(-(value + 1)) + 1u;
}

std::int32_t zigzag_decode(std::uint32_t value) noexcept {
    const auto half = static_cast<std::int32_t>(value >> 1);
    return (value & 1u) != 0 ? -1 - half : half;
}

int bit_length(std::uint32_t value) noexcept {
    int length = 0;
    while (value != 0) {
        value >>= 1;
        ++length;
    }
    return length;
}

void encode_value(RangeEncoder& encoder, AdaptiveValue& model, std::uint32_t value) {
    const int length = bit_length(value);
    for (int i = 0; i < length; ++i) {
        encoder.encode(model.length[model_index(i)], true);
    }
    if (length < kMaxBitLength) {
        encoder.encode(model.length[model_index(length)], false);
    }
    if (length >= 2) {
        const bool first = ((value >> (length - 2)) & 1u) != 0;
        encoder.encode(model.first_bit[model_index(length)], first);
        encoder.encode_direct(value & ((std::uint32_t{1} << (length - 2)) - 1), length - 2);
    }
}

std::uint32_t decode_value(RangeDecoder& decoder, AdaptiveValue& model) noexcept {
    int length = 0;
    while (length < kMaxBitLength && decoder.decode(model.length[model_index(length)])) {
        ++length;
    }
    if (length < 2) {
        return static_cast<std::uint32_t>(length);
    }
    std::uint32_t value = 1;
    value = (value << 1) | (decoder.decode(model.first_bit[model_index(length)]) ? 1u : 0u);
    if (length > 2) {
        value = (value << (length - 2)) | decoder.decode_direct(length - 2);
    }
    return value;
}

}  // namespace codec
