#include <iostream>

#include "codec/version.hpp"

int main() {
    std::cerr << "codec-encode " << codec::version() << ": not implemented yet\n"
              << "planned usage: codec-encode in.wav out.bin --bitrate 64000\n";
    return 2;
}
