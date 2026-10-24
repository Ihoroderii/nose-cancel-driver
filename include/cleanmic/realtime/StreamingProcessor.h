#pragma once

#include "cleanmic/processing/HushEnhancer.h"
#include "cleanmic/processing/RnnoiseEnhancer.h"

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

namespace cleanmic::realtime {

enum class LiveBackend {
    hush,
    rnnoise,
    bypass,
};

inline std::string live_backend_name(LiveBackend backend) {
    switch (backend) {
        case LiveBackend::hush:
            return "hush";
        case LiveBackend::rnnoise:
            return "rnnoise";
        case LiveBackend::bypass:
            return "bypass";
    }
    return "unknown";
}

class StreamingProcessor {
public:
    StreamingProcessor(LiveBackend backend, int sample_rate, float hush_attenuation_limit_db = 100.0f)
        : backend_(backend), sample_rate_(sample_rate) {
        switch (backend_) {
            case LiveBackend::hush: {
                cleanmic::processing::HushEnhancerConfig config;
                config.sample_rate = sample_rate_;
                config.attenuation_limit_db = hush_attenuation_limit_db;
                hush_ = cleanmic::processing::HushEnhancer(config);
                frame_size_ = hush_->frame_size();
                break;
            }
            case LiveBackend::rnnoise: {
                cleanmic::processing::RnnoiseEnhancerConfig config;
                config.sample_rate = sample_rate_;
                rnnoise_ = cleanmic::processing::RnnoiseEnhancer(config);
                frame_size_ = rnnoise_->frame_size();
                break;
            }
            case LiveBackend::bypass:
                frame_size_ = std::max<std::size_t>(static_cast<std::size_t>(sample_rate_ / 100), 160);
                break;
        }

        if (frame_size_ == 0) {
            throw std::runtime_error("StreamingProcessor frame size resolved to zero.");
        }

        frame_output_.resize(frame_size_, 0.0f);
    }

    std::size_t frame_size() const {
        return frame_size_;
    }

    int sample_rate() const {
        return sample_rate_;
    }

    void reset() {
        input_queue_.clear();
        input_offset_ = 0;
        output_queue_.clear();
        output_offset_ = 0;

        if (hush_.has_value()) {
            hush_->reset();
        }
        if (rnnoise_.has_value()) {
            rnnoise_->reset();
        }
    }

    void push_input(const float* input, std::size_t count) {
        input_queue_.insert(input_queue_.end(), input, input + count);

        while ((input_queue_.size() - input_offset_) >= frame_size_) {
            const float* frame = input_queue_.data() + input_offset_;

            switch (backend_) {
                case LiveBackend::hush:
                    hush_->process_frame(frame, frame_output_.data(), frame_size_);
                    break;
                case LiveBackend::rnnoise:
                    rnnoise_->process_frame(frame, frame_output_.data(), frame_size_);
                    break;
                case LiveBackend::bypass:
                    std::copy(frame, frame + frame_size_, frame_output_.begin());
                    break;
            }

            output_queue_.insert(output_queue_.end(), frame_output_.begin(), frame_output_.end());
            input_offset_ += frame_size_;
        }

        compact_queue(input_queue_, input_offset_);
    }

    std::size_t available_output() const {
        return output_queue_.size() - output_offset_;
    }

    std::size_t pop_output(float* output, std::size_t count, bool zero_fill = true) {
        const std::size_t available = available_output();
        const std::size_t to_copy = std::min(count, available);
        std::copy_n(output_queue_.data() + output_offset_, to_copy, output);
        if (zero_fill) {
            std::fill(output + to_copy, output + count, 0.0f);
        }
        output_offset_ += to_copy;
        compact_queue(output_queue_, output_offset_);
        return to_copy;
    }

    void process(const float* input, std::size_t count, std::vector<float>& output) {
        push_input(input, count);
        output.resize(count);
        (void)pop_output(output.data(), count, true);
    }

private:
    static void compact_queue(std::vector<float>& queue, std::size_t& offset) {
        if (offset == 0) {
            return;
        }

        if (offset >= queue.size()) {
            queue.clear();
            offset = 0;
            return;
        }

        if (offset >= 4096 || offset * 2 >= queue.size()) {
            queue.erase(queue.begin(), queue.begin() + static_cast<std::ptrdiff_t>(offset));
            offset = 0;
        }
    }

    LiveBackend backend_;
    int sample_rate_ = 0;
    std::size_t frame_size_ = 0;
    std::optional<cleanmic::processing::HushEnhancer> hush_;
    std::optional<cleanmic::processing::RnnoiseEnhancer> rnnoise_;
    std::vector<float> input_queue_;
    std::size_t input_offset_ = 0;
    std::vector<float> output_queue_;
    std::size_t output_offset_ = 0;
    std::vector<float> frame_output_;
};

}  // namespace cleanmic::realtime
