#include "processing/FFT.h"

#include <cmath>
#include <stdexcept>
#include <utility>

namespace cleanmic::processing {
namespace {

constexpr float kPi = 3.14159265358979323846f;

bool is_power_of_two(std::size_t value) {
    return value > 0 && (value & (value - 1)) == 0;
}

std::size_t reverse_bits(std::size_t value, unsigned int bits) {
    std::size_t reversed = 0;
    for (unsigned int i = 0; i < bits; ++i) {
        reversed = (reversed << 1u) | ((value >> i) & 1u);
    }
    return reversed;
}

}  // namespace

FFT::FFT(std::size_t size) : size_(size), bit_reversed_(size) {
    if (!is_power_of_two(size_)) {
        throw std::invalid_argument("FFT size must be a power of two.");
    }

    unsigned int bits = 0;
    while ((static_cast<std::size_t>(1) << bits) < size_) {
        ++bits;
    }

    for (std::size_t i = 0; i < size_; ++i) {
        bit_reversed_[i] = reverse_bits(i, bits);
    }
}

std::size_t FFT::size() const {
    return size_;
}

void FFT::forward(std::vector<std::complex<float>>& data) const {
    transform(data, false);
}

void FFT::inverse(std::vector<std::complex<float>>& data) const {
    transform(data, true);
}

void FFT::transform(std::vector<std::complex<float>>& data, bool inverse) const {
    if (data.size() != size_) {
        throw std::invalid_argument("FFT input size does not match configured FFT size.");
    }

    for (std::size_t i = 0; i < size_; ++i) {
        const std::size_t j = bit_reversed_[i];
        if (j > i) {
            std::swap(data[i], data[j]);
        }
    }

    for (std::size_t length = 2; length <= size_; length <<= 1u) {
        const float angle = (inverse ? 2.0f : -2.0f) * kPi / static_cast<float>(length);
        const std::complex<float> phase_step(std::cos(angle), std::sin(angle));

        for (std::size_t offset = 0; offset < size_; offset += length) {
            std::complex<float> phase(1.0f, 0.0f);
            const std::size_t half = length / 2;

            for (std::size_t i = 0; i < half; ++i) {
                const std::complex<float> even = data[offset + i];
                const std::complex<float> odd = data[offset + i + half] * phase;

                data[offset + i] = even + odd;
                data[offset + i + half] = even - odd;
                phase *= phase_step;
            }
        }
    }

    if (inverse) {
        const float scale = 1.0f / static_cast<float>(size_);
        for (auto& value : data) {
            value *= scale;
        }
    }
}

}  // namespace cleanmic::processing

