#include "codec/bitstream.hpp"

namespace codec {
namespace {

constexpr int kMaxBitsPerCall = 32;

void check_count(int count) {
    if (count < 0 || count > kMaxBitsPerCall) {
        throw std::invalid_argument("the bit count must be in 0 to 32");
    }
}

}  // namespace

void BitWriter::write_bit(bool bit) {
    const std::size_t offset = bit_count_ % 8;
    if (offset == 0) {
        bytes_.push_back(0);
    }
    if (bit) {
        bytes_.back() = static_cast<std::uint8_t>(bytes_.back() | (0x80u >> offset));
    }
    ++bit_count_;
}

void BitWriter::write_bits(std::uint32_t value, int count) {
    check_count(count);
    if (count < kMaxBitsPerCall && (value >> count) != 0) {
        throw std::invalid_argument("the value does not fit into the bit count");
    }
    for (int i = count - 1; i >= 0; --i) {
        write_bit(((value >> i) & 1u) != 0);
    }
}

void BitWriter::align_to_byte() {
    while (bit_count_ % 8 != 0) {
        write_bit(false);
    }
}

BitReader::BitReader(const std::uint8_t* data, std::size_t size) noexcept
    : data_(data), bit_size_(size * 8) {}

bool BitReader::read_bit() {
    if (position_ >= bit_size_) {
        throw BitstreamError("the bitstream ends too early");
    }
    const std::uint8_t byte = data_[position_ / 8];
    const bool bit = ((byte >> (7 - position_ % 8)) & 1u) != 0;
    ++position_;
    return bit;
}

std::uint32_t BitReader::read_bits(int count) {
    check_count(count);
    std::uint32_t value = 0;
    for (int i = 0; i < count; ++i) {
        value = (value << 1) | (read_bit() ? 1u : 0u);
    }
    return value;
}

void BitReader::skip_padding() {
    while (position_ % 8 != 0) {
        if (read_bit()) {
            throw BitstreamError("the padding bits are not zero");
        }
    }
}

}  // namespace codec
