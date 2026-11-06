#include <cstddef>
#include <exception>
#include <fstream>
#include <iostream>
#include <limits>
#include <locale>
#include <stdexcept>
#include <vector>

#include "codec/codec.hpp"
#include "codec/version.hpp"
#include "codec/wav.hpp"
#include "encoder_arguments.hpp"

namespace {

constexpr int kExitError = 1;
constexpr int kExitUsage = 2;

int usage() {
    std::cerr << "codec-analyze " << codec::version() << "\n"
              << "usage: codec-analyze in.wav out.csv --bitrate 64000 [--psy on|off]\n"
              << "Writes one line per frame and band: the energy, thresholds and noise that the\n"
              << "encoder sees, on the scale of the MDCT coefficients. See eval/README.md.\n";
    return kExitUsage;
}

}  // namespace

int main(int argc, char** argv) {
    tools::EncoderArguments arguments;
    if (!tools::parse_encoder_arguments(argc, argv, arguments)) {
        return usage();
    }

    try {
        const codec::WavAudio audio = codec::read_wav(arguments.files[0]);
        const std::vector<codec::FrameReport> reports =
            codec::report_encoding(audio, arguments.options);

        std::ofstream file(arguments.files[1], std::ios::trunc);
        if (!file) {
            throw std::runtime_error("cannot create " + arguments.files[1]);
        }
        file.imbue(std::locale::classic());
        file.precision(std::numeric_limits<codec::Sample>::max_digits10);
        file << "frame,band,first_coefficient,last_coefficient,energy,masking_threshold,"
                "quiet_threshold,noise,payload_bytes,budget_bits\n";
        for (std::size_t i = 0; i < reports.size(); ++i) {
            const codec::FrameReport& report = reports[i];
            for (std::size_t band = 0; band < codec::kBandCount; ++band) {
                file << i << ',' << band << ',' << codec::kBandEdges[band] << ','
                     << codec::kBandEdges[band + 1] - 1 << ',' << report.band_energy[band] << ','
                     << report.masking_threshold[band] << ',' << report.quiet_threshold[band] << ','
                     << report.noise[band] << ',' << report.payload_bytes << ','
                     << report.budget_bits << '\n';
            }
        }
        file.close();
        if (file.fail()) {
            throw std::runtime_error("cannot write " + arguments.files[1]);
        }
        std::cout << arguments.files[0] << ": " << reports.size() << " frames written to "
                  << arguments.files[1] << "\n";
    } catch (const std::exception& error) {
        std::cerr << "codec-analyze: " << error.what() << "\n";
        return kExitError;
    }
    return 0;
}
