#include "cleanmic/processing/RnnoiseEnhancer.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

#if defined(CLEANMIC_HAS_RNNOISE) && CLEANMIC_HAS_RNNOISE
extern "C" {
#include "rnnoise.h"
}
#endif

namespace cleanmic::processing {
namespace {

constexpr float kPcmScale = 32768.0f;

void limit_peak(std::vector<float>& samples) {
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

struct RnnoiseEnhancer::Impl {
#if defined(CLEANMIC_HAS_RNNOISE) && CLEANMIC_HAS_RNNOISE
    DenoiseState* state = nullptr;
    std::size_t frame_size = 0;
    bool warmup_pending = false;
#endif
};

RnnoiseEnhancer::RnnoiseEnhancer(RnnoiseEnhancerConfig config)
    : config_(config), impl_(std::make_unique<Impl>()) {
    reset();
}

RnnoiseEnhancer::~RnnoiseEnhancer() {
#if defined(CLEANMIC_HAS_RNNOISE) && CLEANMIC_HAS_RNNOISE
    if (impl_ && impl_->state != nullptr) {
        rnnoise_destroy(impl_->state);
        impl_->state = nullptr;
    }
#endif
}

RnnoiseEnhancer::RnnoiseEnhancer(RnnoiseEnhancer&&) noexcept = default;
RnnoiseEnhancer& RnnoiseEnhancer::operator=(RnnoiseEnhancer&&) noexcept = default;

std::vector<float> RnnoiseEnhancer::process(const std::vector<float>& input) {
#if !defined(CLEANMIC_HAS_RNNOISE) || !CLEANMIC_HAS_RNNOISE
    (void)input;
    throw std::runtime_error("RNNoise backend is not available in this build.");
#else
    if (config_.sample_rate != 48000) {
        throw std::runtime_error("RNNoise backend currently requires 48000 Hz input.");
    }

    if (input.empty()) {
        return {};
    }

    reset();

    std::vector<float> output(input.size(), 0.0f);
    std::vector<float> in_frame(impl_->frame_size, 0.0f);

    for (std::size_t offset = 0; offset < input.size(); offset += impl_->frame_size) {
        const std::size_t count = std::min(impl_->frame_size, input.size() - offset);
        std::fill(in_frame.begin(), in_frame.end(), 0.0f);
        for (std::size_t i = 0; i < count; ++i) {
            in_frame[i] = input[offset + i];
        }

        process_frame(in_frame.data(), in_frame.data(), impl_->frame_size);
        std::copy_n(in_frame.begin(), count, output.begin() + static_cast<std::ptrdiff_t>(offset));
    }

    limit_peak(output);
    return output;
#endif
}

void RnnoiseEnhancer::process_frame(const float* input, float* output, std::size_t frame_count) {
#if !defined(CLEANMIC_HAS_RNNOISE) || !CLEANMIC_HAS_RNNOISE
    (void)input;
    (void)output;
    (void)frame_count;
    throw std::runtime_error("RNNoise backend is not available in this build.");
#else
    if (frame_count != impl_->frame_size) {
        throw std::invalid_argument("RNNoise frame size mismatch.");
    }

    std::vector<float> in_frame(frame_count, 0.0f);
    std::vector<float> out_frame(frame_count, 0.0f);
    for (std::size_t i = 0; i < frame_count; ++i) {
        in_frame[i] = std::clamp(input[i], -1.0f, 1.0f) * kPcmScale;
    }

    (void)rnnoise_process_frame(impl_->state, out_frame.data(), in_frame.data());

    for (std::size_t i = 0; i < frame_count; ++i) {
        output[i] = impl_->warmup_pending ? 0.0f : out_frame[i] / kPcmScale;
    }
    impl_->warmup_pending = false;
#endif
}

void RnnoiseEnhancer::reset() {
#if !defined(CLEANMIC_HAS_RNNOISE) || !CLEANMIC_HAS_RNNOISE
    throw std::runtime_error("RNNoise backend is not available in this build.");
#else
    if (config_.sample_rate != 48000) {
        throw std::invalid_argument("RNNoise backend currently requires 48000 Hz input.");
    }

    if (impl_->state != nullptr) {
        rnnoise_destroy(impl_->state);
        impl_->state = nullptr;
    }

    impl_->frame_size = static_cast<std::size_t>(rnnoise_get_frame_size());
    impl_->state = rnnoise_create(nullptr);
    if (impl_->state == nullptr) {
        throw std::runtime_error("Failed to create RNNoise state.");
    }
    impl_->warmup_pending = config_.zero_warmup_frame;
#endif
}

std::size_t RnnoiseEnhancer::frame_size() const {
#if !defined(CLEANMIC_HAS_RNNOISE) || !CLEANMIC_HAS_RNNOISE
    return 0;
#else
    return impl_->frame_size;
#endif
}

int RnnoiseEnhancer::required_sample_rate() const {
    return 48000;
}

}  // namespace cleanmic::processing
