#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace codec {

/// Probabilities are given in units of 1 / 4096, as the chance that a binary decision is 0. They
/// must be in 1 to 4095.
inline constexpr std::uint32_t kProbabilityBits = 12;
inline constexpr std::uint32_t kProbabilityOne = 1u << kProbabilityBits;

/// A probability that learns from the decisions it has seen. It is the mean of two estimates: one
/// adapts fast, the other is slower and more precise. It uses only integers, so it evolves in the
/// same way on every platform.
class AdaptiveBit {
public:
    std::uint32_t probability_of_zero() const noexcept;
    void update(bool bit) noexcept;

private:
    std::uint16_t fast_ = 1u << 15;
    std::uint16_t slow_ = 1u << 15;
};

/// Binary range encoder, in the style of the one in LZMA. Unlike a code with whole code words, it
/// can spend far less than one bit on a decision that is likely.
class RangeEncoder {
public:
    /// Codes a decision with a fixed probability.
    void encode(std::uint32_t probability_of_zero, bool bit);

    /// Codes a decision with an adaptive probability and updates it.
    void encode(AdaptiveBit& model, bool bit);

    /// Codes the lowest count bits of value at one bit each, most significant first.
    void encode_direct(std::uint32_t value, int count);

    /// Ends the code and returns the bytes. Trailing zero bytes are left out, because the decoder
    /// reads zeros past the end anyway, so the last byte is never zero.
    std::vector<std::uint8_t> finish();

private:
    void normalize();
    void shift_low();
    void emit(std::uint8_t byte);

    std::uint64_t low_ = 0;
    std::uint32_t range_ = 0xFFFFFFFFu;
    std::uint8_t cache_ = 0;
    std::uint64_t cache_size_ = 1;
    bool first_byte_ = true;
    std::vector<std::uint8_t> bytes_;
};

/// Inverse of RangeEncoder. It reads zeros past the end of the data, so any data can be decoded
/// without a crash. Finding out whether the decoded values make sense is up to the caller. The data
/// must stay valid while the decoder is used.
class RangeDecoder {
public:
    RangeDecoder(const std::uint8_t* data, std::size_t size) noexcept;

    bool decode(std::uint32_t probability_of_zero) noexcept;
    bool decode(AdaptiveBit& model) noexcept;
    std::uint32_t decode_direct(int count) noexcept;

    /// False if the decoder has not yet read all of the data. In a correct stream, the decoder
    /// always reads at least as far as the encoder has written.
    bool has_read_all_data() const noexcept { return position_ >= size_; }

private:
    void normalize() noexcept;
    std::uint8_t next_byte() noexcept;

    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t position_ = 0;
    std::uint32_t code_ = 0;
    std::uint32_t range_ = 0xFFFFFFFFu;
};

}  // namespace codec
