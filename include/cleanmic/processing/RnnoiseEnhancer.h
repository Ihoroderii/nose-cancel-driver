#pragma once

#include <cstddef>
#include <memory>
#include <vector>

namespace cleanmic::processing {

struct RnnoiseEnhancerConfig {
    int sample_rate = 48000;
    bool zero_warmup_frame = true;
};

class RnnoiseEnhancer {
public:
    explicit RnnoiseEnhancer(RnnoiseEnhancerConfig config = {});
    ~RnnoiseEnhancer();

    RnnoiseEnhancer(RnnoiseEnhancer&&) noexcept;
    RnnoiseEnhancer& operator=(RnnoiseEnhancer&&) noexcept;

    RnnoiseEnhancer(const RnnoiseEnhancer&) = delete;
    RnnoiseEnhancer& operator=(const RnnoiseEnhancer&) = delete;

    std::vector<float> process(const std::vector<float>& input);
    void process_frame(const float* input, float* output, std::size_t frame_count);
    void reset();
    std::size_t frame_size() const;
    int required_sample_rate() const;

private:
    struct Impl;

    RnnoiseEnhancerConfig config_;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cleanmic::processing
