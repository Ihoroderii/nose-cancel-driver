#pragma once

#include <cstddef>
#include <memory>
#include <string>

namespace cleanmic::platform {

class VirtualMicrophone {
public:
    virtual ~VirtualMicrophone() = default;

    virtual bool open(int sample_rate, int channels) = 0;
    virtual bool write(const float* samples, std::size_t frame_count) = 0;
    virtual void close() = 0;
    virtual std::string diagnostic() const = 0;
};

std::unique_ptr<VirtualMicrophone> create_virtual_microphone();

}  // namespace cleanmic::platform

