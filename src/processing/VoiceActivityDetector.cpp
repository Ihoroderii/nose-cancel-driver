#include "cleanmic/processing/VoiceActivityDetector.h"

#include <algorithm>

namespace cleanmic::processing {

VoiceActivityDetector::VoiceActivityDetector(VoiceActivityDetectorConfig config) : config_(config) {}

bool VoiceActivityDetector::is_speech(float rms) {
    const float energy = std::max(rms * rms, 1.0e-8f);

    if (frames_seen_ < config_.bootstrap_frames) {
        noise_floor_energy_ =
            (frames_seen_ == 0) ? energy : 0.8f * noise_floor_energy_ + 0.2f * energy;
        ++frames_seen_;
        return false;
    }

    const bool speech = energy > noise_floor_energy_ * config_.speech_ratio;

    if (speech) {
        noise_floor_energy_ =
            0.999f * noise_floor_energy_ + 0.001f * std::min(energy, noise_floor_energy_);
    } else {
        noise_floor_energy_ = config_.noise_smoothing * noise_floor_energy_ +
                              (1.0f - config_.noise_smoothing) * energy;
    }

    ++frames_seen_;
    return speech;
}

float VoiceActivityDetector::noise_floor() const {
    return noise_floor_energy_;
}

void VoiceActivityDetector::reset() {
    noise_floor_energy_ = 1.0e-6f;
    frames_seen_ = 0;
}

}  // namespace cleanmic::processing

