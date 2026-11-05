#include <gtest/gtest.h>

#include <string>

#include "codec/version.hpp"

// Placeholder: shows that the library links. Remove it when the first real module has tests.
TEST(Version, IsNotEmpty) { EXPECT_FALSE(std::string(codec::version()).empty()); }
