#include "codec/range_coder.hpp"

namespace codec {
namespace {

constexpr std::uint32_t kTopValue = 1u << 24;
constexpr int kFastRate = 4;
constexpr int kSlowRate = 7;
constexpr std::uint32_t kMaxAdaptive = 0xFFFFu;

std::uint16_t moved_towards(std::uint16_t value, bool bit, int rate) noexcept {
    const std::uint32_t wide = value;
    return static_cast<std::uint16_t>(bit ? wide - (wide >> rate)
                                          : wide + ((kMaxAdaptive - wide) >> rate));
}

}  // namespace

std::uint32_t AdaptiveBit::probability_of_zero() const noexcept {
    const std::uint32_t probability = (std::uint32_t{fast_} + std::uint32_t{slow_}) >> 5;
    if (probability < 1) {
        return 1;
    }
    return probability >= kProbabilityOne ? kProbabilityOne - 1 : probability;
}

void AdaptiveBit::update(bool bit) noexcept {
    fast_ = moved_towards(fast_, bit, kFastRate);
    slow_ = moved_towards(slow_, bit, kSlowRate);
}

void RangeEncoder::emit(std::uint8_t byte) {
    // The first byte is always zero, since nothing can carry into it. It is left out.
    if (first_byte_) {
        first_byte_ = false;
        return;
    }
    bytes_.push_back(byte);
}

void RangeEncoder::shift_low() {
    if (static_cast<std::uint32_t>(low_) < 0xFF000000u || (low_ >> 32) != 0) {
        const auto carry = static_cast<std::uint8_t>(low_ >> 32);
        std::uint8_t byte = cache_;
        do {
            emit(static_cast<std::uint8_t>(byte + carry));
            byte = 0xFF;
        } while (--cache_size_ != 0);
        cache_ = static_cast<std::uint8_t>((low_ >> 24) & 0xFFu);
    }
    ++cache_size_;
    low_ = (low_ & 0x00FFFFFFu) << 8;
}

void RangeEncoder::normalize() {
    while (range_ < kTopValue) {
        range_ <<= 8;
        shift_low();
    }
}

void RangeEncoder::encode(std::uint32_t probability_of_zero, bool bit) {
    const std::uint32_t bound = (range_ >> kProbabilityBits) * probability_of_zero;
    if (bit) {
        low_ += bound;
        range_ -= bound;
    } else {
        range_ = bound;
    }
    normalize();
}

void RangeEncoder::encode(AdaptiveBit& model, bool bit) {
    encode(model.probability_of_zero(), bit);
    model.update(bit);
}

void RangeEncoder::encode_direct(std::uint32_t value, int count) {
    for (int i = count - 1; i >= 0; --i) {
        range_ >>= 1;
        if (((value >> i) & 1u) != 0) {
            low_ += range_;
        }
        normalize();
    }
}

std::vector<std::uint8_t> RangeEncoder::finish() {
    // Any value in low_ to low_ + range_ decodes correctly, so this takes the one with the most
    // trailing zeros that it can find at a whole byte. The range is at least 2^24, so rounding up
    // to a multiple of 2^24 always stays inside.
    const std::uint64_t end = low_ + range_;
    const std::uint64_t multiple_32 = ((low_ + 0xFFFFFFFFu) >> 32) << 32;
    const std::uint64_t multiple_24 = ((low_ + 0xFFFFFFu) >> 24) << 24;
    low_ = multiple_32 < end ? multiple_32 : multiple_24;

    for (int i = 0; i < 5; ++i) {
        shift_low();
    }
    while (!bytes_.empty() && bytes_.back() == 0) {
        bytes_.pop_back();
    }
    return bytes_;
}

RangeDecoder::RangeDecoder(const std::uint8_t* data, std::size_t size) noexcept
    : data_(data), size_(size) {
    for (int i = 0; i < 4; ++i) {
        code_ = (code_ << 8) | next_byte();
    }
}

std::uint8_t RangeDecoder::next_byte() noexcept {
    const std::uint8_t byte = position_ < size_ ? data_[position_] : std::uint8_t{0};
    ++position_;
    return byte;
}

void RangeDecoder::normalize() noexcept {
    while (range_ < kTopValue) {
        range_ <<= 8;
        code_ = (code_ << 8) | next_byte();
    }
}

bool RangeDecoder::decode(std::uint32_t probability_of_zero) noexcept {
    const std::uint32_t bound = (range_ >> kProbabilityBits) * probability_of_zero;
    const bool bit = code_ >= bound;
    if (bit) {
        code_ -= bound;
        range_ -= bound;
    } else {
        range_ = bound;
    }
    normalize();
    return bit;
}

bool RangeDecoder::decode(AdaptiveBit& model) noexcept {
    const bool bit = decode(model.probability_of_zero());
    model.update(bit);
    return bit;
}

std::uint32_t RangeDecoder::decode_direct(int count) noexcept {
    std::uint32_t value = 0;
    for (int i = 0; i < count; ++i) {
        range_ >>= 1;
        const bool bit = code_ >= range_;
        if (bit) {
            code_ -= range_;
        }
        value = (value << 1) | (bit ? 1u : 0u);
        normalize();
    }
    return value;
}

}  // namespace codec
