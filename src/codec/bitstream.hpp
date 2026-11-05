#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace codec {

/// Thrown when a bitstream is damaged or ends too early.
class BitstreamError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Writes bits starting with the most significant bit of each byte.
class BitWriter {
public:
    void write_bit(bool bit);

    /// Writes the lowest count bits of value, most significant first. Throws std::invalid_argument
    /// if count is not in 0 to 32 or value does not fit into count bits.
    void write_bits(std::uint32_t value, int count);

    /// Fills up the current byte with zero bits.
    void align_to_byte();

    std::size_t bit_count() const noexcept { return bit_count_; }
    const std::vector<std::uint8_t>& bytes() const noexcept { return bytes_; }

private:
    std::vector<std::uint8_t> bytes_;
    std::size_t bit_count_ = 0;
};

/// Reads bits in the order of BitWriter. The data must stay valid while the reader is used. Reading
/// past the end throws BitstreamError.
class BitReader {
public:
    BitReader(const std::uint8_t* data, std::size_t size) noexcept;

    bool read_bit();

    /// Reads count bits, most significant first. Throws std::invalid_argument if count is not in
    /// 0 to 32.
    std::uint32_t read_bits(int count);

    /// Skips to the next byte boundary. Throws BitstreamError if a skipped bit is not zero.
    void skip_padding();

    std::size_t bits_remaining() const noexcept { return bit_size_ - position_; }

private:
    const std::uint8_t* data_;
    std::size_t bit_size_;
    std::size_t position_ = 0;
};

}  // namespace codec
