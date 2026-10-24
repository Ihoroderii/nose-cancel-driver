#include "cleanmic/io/WavFile.h"
#include "cleanmic/platform/VirtualMicrophone.h"
#include "cleanmic/processing/HushEnhancer.h"
#include "cleanmic/processing/RnnoiseEnhancer.h"
#include "cleanmic/processing/VoiceEnhancer.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

namespace {

enum class BackendMode {
    dsp,
    hush,
    rnnoise,
    both,
    all,
};

struct Options {
    std::string input_path;
    std::string output_path;
    BackendMode mode = BackendMode::hush;
    bool show_virtual_mic_status = false;
};

std::string to_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

BackendMode parse_mode(const std::string& value) {
    const std::string lowered = to_lower(value);
    if (lowered == "dsp") {
        return BackendMode::dsp;
    }
    if (lowered == "rnnoise") {
        return BackendMode::rnnoise;
    }
    if (lowered == "hush") {
        return BackendMode::hush;
    }
    if (lowered == "both") {
        return BackendMode::both;
    }
    if (lowered == "all") {
        return BackendMode::all;
    }

    throw std::runtime_error("Unknown mode: " + value + ". Use dsp, hush, rnnoise, both, or all.");
}

std::string add_suffix_to_path(const std::string& path, const std::string& suffix) {
    const std::size_t last_separator = path.find_last_of("/\\");
    const std::size_t last_dot = path.find_last_of('.');
    if (last_dot == std::string::npos ||
        (last_separator != std::string::npos && last_dot < last_separator)) {
        return path + suffix + ".wav";
    }

    return path.substr(0, last_dot) + suffix + path.substr(last_dot);
}

void write_output_wav(const std::string& path, int sample_rate, const std::vector<float>& samples) {
    cleanmic::io::WavData output_wav;
    output_wav.sample_rate = sample_rate;
    output_wav.channels = 1;
    output_wav.samples = samples;
    cleanmic::io::write_wav_file(path, output_wav);
}

void print_usage() {
    std::cout << "Usage:\n"
              << "  cleanmic_cli --input noisy.wav --output clean.wav [--mode dsp|hush|rnnoise|both|all]\n"
              << "    default mode: hush\n"
              << "  cleanmic_cli --show-virtual-mic-status\n";
}

Options parse_options(int argc, char** argv) {
    Options options;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--input" && i + 1 < argc) {
            options.input_path = argv[++i];
        } else if (arg == "--output" && i + 1 < argc) {
            options.output_path = argv[++i];
        } else if (arg == "--mode" && i + 1 < argc) {
            options.mode = parse_mode(argv[++i]);
        } else if (arg == "--show-virtual-mic-status") {
            options.show_virtual_mic_status = true;
        } else if (arg == "--help" || arg == "-h") {
            print_usage();
            std::exit(0);
        } else {
            throw std::runtime_error("Unknown or incomplete argument: " + arg);
        }
    }

    return options;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);

        if (options.show_virtual_mic_status) {
            auto virtual_mic = cleanmic::platform::create_virtual_microphone();
            std::cout << virtual_mic->diagnostic() << '\n';
            return 0;
        }

        if (options.input_path.empty() || options.output_path.empty()) {
            print_usage();
            return 1;
        }

        const cleanmic::io::WavData input_wav = cleanmic::io::read_wav_file(options.input_path);
        const std::vector<float> mono = cleanmic::io::to_mono(input_wav);

        std::cout << "Input sample rate: " << input_wav.sample_rate << " Hz\n";
        std::cout << "Input channels: " << input_wav.channels << '\n';

        if (options.mode == BackendMode::dsp || options.mode == BackendMode::both ||
            options.mode == BackendMode::all) {
            cleanmic::processing::VoiceEnhancerConfig config;
            config.sample_rate = input_wav.sample_rate;
            cleanmic::processing::VoiceEnhancer enhancer(config);
            const std::vector<float> cleaned = enhancer.process(mono);
            const std::string output_path = (options.mode == BackendMode::both ||
                                             options.mode == BackendMode::all)
                                                ? add_suffix_to_path(options.output_path, "_dsp")
                                                : options.output_path;
            write_output_wav(output_path, input_wav.sample_rate, cleaned);
            std::cout << "DSP output: " << output_path << '\n';
        }

        if (options.mode == BackendMode::hush || options.mode == BackendMode::all) {
#if !defined(CLEANMIC_HAS_HUSH) || !CLEANMIC_HAS_HUSH
            throw std::runtime_error("Hush backend is not available in this build.");
#else
            cleanmic::processing::HushEnhancerConfig config;
            config.sample_rate = input_wav.sample_rate;
            cleanmic::processing::HushEnhancer enhancer(config);
            const std::vector<float> cleaned = enhancer.process(mono);
            const std::string output_path = (options.mode == BackendMode::all)
                                                ? add_suffix_to_path(options.output_path, "_hush")
                                                : options.output_path;
            write_output_wav(output_path, input_wav.sample_rate, cleaned);
            std::cout << "Hush output: " << output_path << '\n';
#endif
        }

        if (options.mode == BackendMode::rnnoise || options.mode == BackendMode::both ||
            options.mode == BackendMode::all) {
#if !defined(CLEANMIC_HAS_RNNOISE) || !CLEANMIC_HAS_RNNOISE
            throw std::runtime_error("RNNoise backend is not available in this build.");
#else
            cleanmic::processing::RnnoiseEnhancerConfig config;
            config.sample_rate = input_wav.sample_rate;
            cleanmic::processing::RnnoiseEnhancer enhancer(config);
            const std::vector<float> cleaned = enhancer.process(mono);
            const std::string output_path = (options.mode == BackendMode::both ||
                                             options.mode == BackendMode::all)
                                                ? add_suffix_to_path(options.output_path, "_rnnoise")
                                                : options.output_path;
            write_output_wav(output_path, input_wav.sample_rate, cleaned);
            std::cout << "RNNoise output: " << output_path << '\n';
#endif
        }

        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Error: " << exception.what() << '\n';
        return 1;
    }
}
