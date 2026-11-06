#include "codec/framing.hpp"

#include <algorithm>
#include <stdexcept>

namespace codec {

std::size_t frame_count(std::size_t total_samples, std::size_t coefficient_count) {
    if (coefficient_count == 0) {
        throw std::invalid_argument("the frame length must not be 0");
    }
    const std::size_t rounded_up =
        total_samples / coefficient_count + (total_samples % coefficient_count != 0 ? 1 : 0);
    return rounded_up + 1;
}

std::vector<std::vector<Sample>> split_into_blocks(const std::vector<Sample>& signal,
                                                   std::size_t coefficient_count) {
    const std::size_t m = coefficient_count;
    const std::size_t count = frame_count(signal.size(), m);

    // M zeros in front, then the signal, then zeros up to the end of the last block.
    std::vector<Sample> padded((count + 1) * m, Sample{0});
    std::copy(signal.begin(), signal.end(), padded.begin() + static_cast<std::ptrdiff_t>(m));

    std::vector<std::vector<Sample>> blocks;
    blocks.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto first = padded.begin() + static_cast<std::ptrdiff_t>(i * m);
        blocks.emplace_back(first, first + static_cast<std::ptrdiff_t>(2 * m));
    }
    return blocks;
}

std::vector<std::vector<Sample>> analyze(const std::vector<Sample>& signal, const Mdct& mdct) {
    const std::vector<std::vector<Sample>> blocks =
        split_into_blocks(signal, mdct.coefficient_count());
    std::vector<std::vector<Sample>> frames;
    frames.reserve(blocks.size());
    for (const std::vector<Sample>& block : blocks) {
        frames.push_back(mdct.forward(block));
    }
    return frames;
}

std::vector<Sample> synthesize(const std::vector<std::vector<Sample>>& frames,
                               std::size_t total_samples, const Mdct& mdct) {
    const std::size_t m = mdct.coefficient_count();
    // Checked before anything is allocated, because total_samples may come from a damaged file.
    if (frames.size() != frame_count(total_samples, m)) {
        throw std::invalid_argument("the number of frames does not fit the number of samples");
    }

    std::vector<Sample> padded((frames.size() + 1) * m, Sample{0});
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const std::vector<Sample> block = mdct.inverse(frames[i]);
        for (std::size_t n = 0; n < block.size(); ++n) {
            padded[i * m + n] += block[n];
        }
    }

    const auto first = padded.begin() + static_cast<std::ptrdiff_t>(m);
    return std::vector<Sample>(first, first + static_cast<std::ptrdiff_t>(total_samples));
}

}  // namespace codec
