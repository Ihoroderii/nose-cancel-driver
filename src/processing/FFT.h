#pragma once

#include <complex>
#include <cstddef>
#include <vector>

namespace cleanmic::processing {

class FFT {
public:
    explicit FFT(std::size_t size);

    std::size_t size() const;
    void forward(std::vector<std::complex<float>>& data) const;
    void inverse(std::vector<std::complex<float>>& data) const;

private:
    void transform(std::vector<std::complex<float>>& data, bool inverse) const;

    std::size_t size_;
    std::vector<std::size_t> bit_reversed_;
};

}  // namespace cleanmic::processing

