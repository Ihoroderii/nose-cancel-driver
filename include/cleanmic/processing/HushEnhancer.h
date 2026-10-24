#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace cleanmic::processing {

struct HushEnhancerConfig {
    int sample_rate = 48000;
    float attenuation_limit_db = 100.0f;
    float target_rms = 0.18f;
    float loudness_gate = 0.015f;
    float max_makeup_gain_db = 18.0f;
    float compressor_threshold = 0.45f;
    float compressor_ratio = 4.0f;
    float limiter_ceiling = 0.98f;
    std::string library_path;
    std::string model_path;
};

class HushEnhancer {
public:
    explicit HushEnhancer(HushEnhancerConfig config = {});
    ~HushEnhancer();

    HushEnhancer(HushEnhancer&&) noexcept;
    HushEnhancer& operator=(HushEnhancer&&) noexcept;

    HushEnhancer(const HushEnhancer&) = delete;
    HushEnhancer& operator=(const HushEnhancer&) = delete;

    std::vector<float> process(const std::vector<float>& input);
    void process_frame(const float* input, float* output, std::size_t frame_count);
    void reset();
    std::size_t frame_size() const;
    int internal_sample_rate() const;

private:
    struct Impl;

    HushEnhancerConfig config_;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cleanmic::processing
