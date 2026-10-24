#include "cleanmic/processing/VoiceEnhancer.h"

#include "processing/FFT.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace cleanmic::processing {
namespace {

constexpr float kPi = 3.14159265358979323846f;

bool is_power_of_two(std::size_t value) {
    return value > 0 && (value & (value - 1)) == 0;
}

float compute_rms(const std::vector<float>& samples, std::size_t offset, std::size_t count) {
    if (count == 0) {
        return 0.0f;
    }

    float accum = 0.0f;
    for (std::size_t i = 0; i < count; ++i) {
        const float sample = samples[offset + i];
        accum += sample * sample;
    }
    return std::sqrt(accum / static_cast<float>(count));
}

void normalize_peak(std::vector<float>& samples) {
    float peak = 0.0f;
    for (float sample : samples) {
        peak = std::max(peak, std::fabs(sample));
    }

    if (peak > 0.99f) {
        const float scale = 0.99f / peak;
        for (float& sample : samples) {
            sample *= scale;
        }
    }
}

}  // namespace

VoiceEnhancer::VoiceEnhancer(VoiceEnhancerConfig config) : config_(config) {
    if (!is_power_of_two(config_.fft_size)) {
        throw std::invalid_argument("VoiceEnhancer fft_size must be a power of two.");
    }
    if (config_.hop_size == 0 || config_.hop_size > config_.fft_size) {
        throw std::invalid_argument("VoiceEnhancer hop_size must be in the range (0, fft_size].");
    }
    if (config_.sample_rate <= 0) {
        throw std::invalid_argument("VoiceEnhancer sample_rate must be positive.");
    }

    build_window();
    reset_state();
}

std::vector<float> VoiceEnhancer::process(const std::vector<float>& input) {
    if (input.empty()) {
        return {};
    }

    reset();

    std::vector<float> filtered(input.size(), 0.0f);
    for (std::size_t i = 0; i < input.size(); ++i) {
        filtered[i] = high_pass_sample(input[i]);
    }

    std::vector<float> padded = filtered;
    if (padded.size() < config_.fft_size) {
        padded.resize(config_.fft_size, 0.0f);
    } else {
        const std::size_t tail = (padded.size() - config_.fft_size) % config_.hop_size;
        if (tail != 0) {
            padded.resize(padded.size() + (config_.hop_size - tail), 0.0f);
        }
    }
    padded.resize(padded.size() + config_.fft_size, 0.0f);

    const std::size_t frame_count =
        1 + (padded.size() - config_.fft_size) / config_.hop_size;
    const std::size_t bins = config_.fft_size / 2 + 1;

    FFT fft(config_.fft_size);
    VoiceActivityDetector vad(config_.vad);
    std::vector<float> noise_profile(bins, 1.0e-6f);
    previous_gain_.assign(bins, 1.0f);

    std::vector<float> output(padded.size(), 0.0f);
    std::vector<float> window_accum(padded.size(), 0.0f);
    std::vector<std::complex<float>> spectrum(config_.fft_size);

    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        const std::size_t offset = frame * config_.hop_size;
        const float rms = compute_rms(padded, offset, config_.fft_size);
        const bool speech = vad.is_speech(rms);
        const bool update_noise = frame < config_.bootstrap_frames || !speech;

        for (std::size_t i = 0; i < config_.fft_size; ++i) {
            spectrum[i] = std::complex<float>(padded[offset + i] * window_[i], 0.0f);
        }

        fft.forward(spectrum);

        for (std::size_t bin = 0; bin < bins; ++bin) {
            const float power = std::norm(spectrum[bin]);

            if (update_noise) {
                noise_profile[bin] = (frame == 0)
                                         ? power
                                         : config_.noise_smoothing * noise_profile[bin] +
                                               (1.0f - config_.noise_smoothing) * power;
            }

            const float snr = power / (noise_profile[bin] + 1.0e-8f);
            float target_gain = snr / (snr + config_.oversubtraction);
            target_gain = std::clamp(target_gain, config_.suppression_floor, 1.0f);

            if (!speech) {
                target_gain = std::min(target_gain, config_.silence_max_gain);
            }

            const float smoothing = (target_gain < previous_gain_[bin])
                                        ? config_.suppression_attack
                                        : config_.suppression_release;
            const float gain =
                smoothing * previous_gain_[bin] + (1.0f - smoothing) * target_gain;
            previous_gain_[bin] = gain;

            spectrum[bin] *= gain;
            if (bin != 0 && bin + 1 != bins) {
                spectrum[config_.fft_size - bin] *= gain;
            }
        }

        fft.inverse(spectrum);

        for (std::size_t i = 0; i < config_.fft_size; ++i) {
            const float sample = spectrum[i].real() * window_[i];
            output[offset + i] += sample;
            window_accum[offset + i] += window_[i] * window_[i];
        }
    }

    output.resize(filtered.size());
    window_accum.resize(filtered.size());

    for (std::size_t i = 0; i < output.size(); ++i) {
        if (window_accum[i] > 1.0e-6f) {
            output[i] /= window_accum[i];
        }
    }

    normalize_peak(output);
    return output;
}

void VoiceEnhancer::reset() {
    reset_state();
}

void VoiceEnhancer::build_window() {
    window_.resize(config_.fft_size);
    if (config_.fft_size == 1) {
        window_[0] = 1.0f;
        return;
    }

    for (std::size_t i = 0; i < config_.fft_size; ++i) {
        window_[i] =
            0.5f - 0.5f * std::cos(2.0f * kPi * static_cast<float>(i) /
                                   static_cast<float>(config_.fft_size - 1));
    }
}

void VoiceEnhancer::reset_state() {
    const float dt = 1.0f / static_cast<float>(config_.sample_rate);
    const float rc = 1.0f / (2.0f * kPi * std::max(config_.high_pass_hz, 1.0f));
    hp_alpha_ = rc / (rc + dt);
    previous_input_ = 0.0f;
    previous_output_ = 0.0f;
    previous_gain_.assign(config_.fft_size / 2 + 1, 1.0f);
}

float VoiceEnhancer::high_pass_sample(float sample) {
    const float output = hp_alpha_ * (previous_output_ + sample - previous_input_);
    previous_input_ = sample;
    previous_output_ = output;
    return output;
}

}  // namespace cleanmic::processing

