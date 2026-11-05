#include <iostream>

#include "codec/version.hpp"

int main() {
    std::cerr << "codec-decode " << codec::version() << ": not implemented yet\n"
              << "planned usage: codec-decode out.bin out.wav\n";
    return 2;
}
