#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <vector>

namespace cleanmic::realtime {

class SpscFloatRingBuffer {
public:
    explicit SpscFloatRingBuffer(std::size_t capacity)
        : buffer_(std::max<std::size_t>(capacity, 1)), capacity_(buffer_.size()) {}

    void push(const float* samples, std::size_t count) {
        std::size_t read = read_index_.load(std::memory_order_acquire);
        std::size_t write = write_index_.load(std::memory_order_relaxed);
        const std::size_t used = write - read;

        if (count >= capacity_) {
            samples += count - capacity_;
            count = capacity_;
            read = write;
        } else {
            const std::size_t free_space = capacity_ - used;
            if (count > free_space) {
                read += count - free_space;
                read_index_.store(read, std::memory_order_release);
            }
        }

        for (std::size_t i = 0; i < count; ++i) {
            buffer_[(write + i) % capacity_] = samples[i];
        }

        write_index_.store(write + count, std::memory_order_release);
    }

    std::size_t pop(float* output, std::size_t count) {
        const std::size_t read = read_index_.load(std::memory_order_relaxed);
        const std::size_t write = write_index_.load(std::memory_order_acquire);
        const std::size_t available = write - read;
        const std::size_t to_read = std::min(count, available);

        for (std::size_t i = 0; i < to_read; ++i) {
            output[i] = buffer_[(read + i) % capacity_];
        }
        for (std::size_t i = to_read; i < count; ++i) {
            output[i] = 0.0f;
        }

        read_index_.store(read + to_read, std::memory_order_release);
        return to_read;
    }

    std::size_t available() const {
        const std::size_t read = read_index_.load(std::memory_order_acquire);
        const std::size_t write = write_index_.load(std::memory_order_acquire);
        return write - read;
    }

private:
    std::vector<float> buffer_;
    std::size_t capacity_ = 0;
    std::atomic<std::size_t> read_index_{0};
    std::atomic<std::size_t> write_index_{0};
};

}  // namespace cleanmic::realtime

