#pragma once

#include <charconv>
#include <cstdint>
#include <string>
#include <system_error>
#include <vector>

#include "codec/codec.hpp"

namespace tools {

/// The command line that codec-encode and codec-analyze share: two file names, --bitrate, --psy and
/// --bandwidth-limit.
struct EncoderArguments {
    std::vector<std::string> files;
    codec::EncoderOptions options;
};

/// Reads a number of bits that is a multiple of 1/8, such as 1, 1.75 or 0.125, as a count of
/// eighths. Only the form is checked here; the encoder checks the range.
inline bool parse_eighths(const std::string& text, std::uint32_t& eighths) {
    const std::size_t point = text.find('.');
    const std::string whole = text.substr(0, point);
    std::string fraction = point == std::string::npos ? "" : text.substr(point + 1);
    if (whole.empty() || (point != std::string::npos && fraction.empty())) {
        return false;
    }
    while (!fraction.empty() && fraction.back() == '0') {
        fraction.pop_back();
    }
    static const char* const kFractions[] = {"", "125", "25", "375", "5", "625", "75", "875"};
    std::uint32_t fraction_eighths = 8;
    for (std::uint32_t i = 0; i < 8; ++i) {
        if (fraction == kFractions[i]) {
            fraction_eighths = i;
        }
    }
    std::uint32_t bits = 0;
    const char* const end = whole.data() + whole.size();
    const auto result = std::from_chars(whole.data(), end, bits);
    if (result.ec != std::errc{} || result.ptr != end || fraction_eighths == 8 || bits > 1000) {
        return false;
    }
    eighths = bits * 8 + fraction_eighths;
    return true;
}

/// Returns false if the arguments are not valid.
inline bool parse_encoder_arguments(int argc, char** argv, EncoderArguments& arguments) {
    bool have_bitrate = false;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--bitrate") {
            if (i + 1 == argc) {
                return false;
            }
            const std::string text = argv[i + 1];
            const char* const end = text.data() + text.size();
            const auto result = std::from_chars(text.data(), end, arguments.options.bitrate);
            if (result.ec != std::errc{} || result.ptr != end) {
                return false;
            }
            have_bitrate = true;
            ++i;
        } else if (argument == "--psy") {
            const std::string value = i + 1 < argc ? argv[i + 1] : "";
            if (value != "on" && value != "off") {
                return false;
            }
            arguments.options.psychoacoustics = value == "on";
            ++i;
        } else if (argument == "--bandwidth-limit") {
            if (i + 1 == argc ||
                !parse_eighths(argv[i + 1], arguments.options.bandwidth_limit_eighths)) {
                return false;
            }
            ++i;
        } else {
            arguments.files.push_back(argument);
        }
    }
    return arguments.files.size() == 2 && have_bitrate;
}

}  // namespace tools
