#pragma once

#include <charconv>
#include <cstdint>
#include <string>
#include <system_error>
#include <vector>

#include "codec/codec.hpp"

namespace tools {

/// The command line that codec-encode and codec-analyze share: two file names, --bitrate and --psy.
struct EncoderArguments {
    std::vector<std::string> files;
    codec::EncoderOptions options;
};

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
        } else {
            arguments.files.push_back(argument);
        }
    }
    return arguments.files.size() == 2 && have_bitrate;
}

}  // namespace tools
