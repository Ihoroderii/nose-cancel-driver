#pragma once

#include <string>
#include <vector>

namespace cleanmic::io {

struct WavData {
    int sample_rate = 0;
    int channels = 0;
    std::vector<float> samples;
};

WavData read_wav_file(const std::string& path);
void write_wav_file(const std::string& path, const WavData& wav);
std::vector<float> to_mono(const WavData& wav);

}  // namespace cleanmic::io

