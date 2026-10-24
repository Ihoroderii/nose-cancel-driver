#pragma once

#include "cleanmic/processing/VoiceActivityDetector.h"

#include <cstddef>
#include <vector>

namespace cleanmic::processing {

struct VoiceEnhancerConfig {
    int sample_rate = 48000;
    std::size_t fft_size = 512;
    std::size_t hop_size = 256;
    float high_pass_hz = 100.0f;
    std::size_t bootstrap_frames = 24;
    float suppression_floor = 0.10f;
    float silence_max_gain = 0.30f;
    float oversubtraction = 1.35f;
    float suppression_attack = 0.20f;
    float suppression_release = 0.85f;
    float noise_smoothing = 0.92f;
    VoiceActivityDetectorConfig vad{};
};

class VoiceEnhancer {
public:
    explicit VoiceEnhancer(VoiceEnhancerConfig config = {});

    std::vector<float> process(const std::vector<float>& input);
    void reset();

private:
    void build_window();
    void reset_state();
    float high_pass_sample(float sample);

    VoiceEnhancerConfig config_;
    std::vector<float> window_;
    std::vector<float> previous_gain_;
    float hp_alpha_ = 0.0f;
    float previous_input_ = 0.0f;
    float previous_output_ = 0.0f;
};

}  // namespace cleanmic::processing
