#pragma once

#include <cstddef>

namespace cleanmic::processing {

struct VoiceActivityDetectorConfig {
    float speech_ratio = 2.4f;
    float noise_smoothing = 0.92f;
    std::size_t bootstrap_frames = 20;
};

class VoiceActivityDetector {
public:
    explicit VoiceActivityDetector(VoiceActivityDetectorConfig config = {});

    bool is_speech(float rms);
    float noise_floor() const;
    void reset();

private:
    VoiceActivityDetectorConfig config_;
    float noise_floor_energy_ = 1.0e-6f;
    std::size_t frames_seen_ = 0;
};

}  // namespace cleanmic::processing

