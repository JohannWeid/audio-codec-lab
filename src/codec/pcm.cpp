#include "codec/pcm.hpp"

#include <cmath>

namespace codec {
namespace {

constexpr Sample kScale = 32768;
constexpr Sample kMaxPcm = 32767;
constexpr Sample kMinPcm = -32768;

std::int16_t to_pcm(Sample sample) {
    if (std::isnan(sample)) {
        return 0;
    }
    const Sample scaled = sample * kScale;
    if (scaled >= kMaxPcm) {
        return 32767;
    }
    if (scaled <= kMinPcm) {
        return -32768;
    }
    // std::lround does not depend on the rounding mode, unlike std::lrint.
    return static_cast<std::int16_t>(std::lround(scaled));
}

}  // namespace

std::vector<Sample> pcm_to_samples(const std::vector<std::int16_t>& pcm) {
    std::vector<Sample> samples;
    samples.reserve(pcm.size());
    for (const std::int16_t value : pcm) {
        samples.push_back(static_cast<Sample>(value) / kScale);
    }
    return samples;
}

std::vector<std::int16_t> samples_to_pcm(const std::vector<Sample>& samples) {
    std::vector<std::int16_t> pcm;
    pcm.reserve(samples.size());
    for (const Sample sample : samples) {
        pcm.push_back(to_pcm(sample));
    }
    return pcm;
}

}  // namespace codec
