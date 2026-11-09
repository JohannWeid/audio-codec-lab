#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "tools/encoder_arguments.hpp"

namespace {

using tools::EncoderArguments;
using tools::parse_eighths;

bool parse(std::vector<std::string> words, EncoderArguments& arguments) {
    words.insert(words.begin(), "codec-encode");
    std::vector<char*> argv;
    for (std::string& word : words) {
        argv.push_back(word.data());
    }
    return tools::parse_encoder_arguments(static_cast<int>(argv.size()), argv.data(), arguments);
}

}  // namespace

TEST(Eighths, AreReadFromMultiplesOfAnEighth) {
    const struct {
        const char* text;
        std::uint32_t eighths;
    } cases[] = {
        {"1", 8},     {"1.0", 8},  {"1.75", 14}, {"1.750", 14}, {"0.125", 1},
        {"0.875", 7}, {"2.5", 20}, {"8", 64},    {"0", 0},      {"12.375", 99},
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(c.text);
        std::uint32_t eighths = 1234;
        ASSERT_TRUE(parse_eighths(c.text, eighths));
        EXPECT_EQ(eighths, c.eighths);
    }
}

TEST(Eighths, RejectEverythingElse) {
    for (const char* text :
         {"", ".", "1.", ".5", "abc", "1.3", "1.33", "-1", "+1", "1.75x", "1,75", " 1", "1001"}) {
        SCOPED_TRACE(text);
        std::uint32_t eighths = 1234;
        EXPECT_FALSE(parse_eighths(text, eighths));
        EXPECT_EQ(eighths, 1234u);
    }
}

TEST(EncoderArguments, ReadTheBandwidthLimit) {
    EncoderArguments arguments;
    ASSERT_TRUE(
        parse({"in.wav", "out.bin", "--bitrate", "32000", "--bandwidth-limit", "1.75"}, arguments));
    EXPECT_EQ(arguments.options.bandwidth_limit_eighths, 14u);
    EXPECT_EQ(arguments.options.bitrate, 32000u);
    EXPECT_TRUE(arguments.options.psychoacoustics);
}

TEST(EncoderArguments, KeepTheDefaultsWithoutOptions) {
    EncoderArguments arguments;
    ASSERT_TRUE(parse({"in.wav", "out.bin", "--bitrate", "64000"}, arguments));
    EXPECT_EQ(arguments.options.bandwidth_limit_eighths, codec::kDefaultBandwidthLimitEighths);
    EXPECT_TRUE(arguments.options.psychoacoustics);
    EXPECT_EQ(arguments.files, (std::vector<std::string>{"in.wav", "out.bin"}));
}

TEST(EncoderArguments, RejectAMissingOrInvalidLimit) {
    for (const std::vector<std::string>& words : {
             std::vector<std::string>{"in.wav", "out.bin", "--bitrate", "1", "--bandwidth-limit"},
             std::vector<std::string>{"in.wav", "out.bin", "--bitrate", "1", "--bandwidth-limit",
                                      "1.3"},
         }) {
        EncoderArguments arguments;
        EXPECT_FALSE(parse(words, arguments));
    }
}
