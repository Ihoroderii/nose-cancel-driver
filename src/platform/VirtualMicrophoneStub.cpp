#include "cleanmic/platform/VirtualMicrophone.h"
#include "cleanmic/ipc/SharedAudioPipe.h"

#include <memory>
#include <string>
#include <utility>

namespace cleanmic::platform {
namespace {

#if defined(__APPLE__)

class SharedPipeVirtualMicrophone final : public VirtualMicrophone {
public:
    bool open(int sample_rate, int channels) override {
        cleanmic::ipc::SharedAudioPipeConfig config;
        config.sample_rate = static_cast<std::uint32_t>(sample_rate);
        config.channels = static_cast<std::uint32_t>(channels);
        config.capacity_frames = cleanmic::ipc::kDefaultSharedAudioPipeCapacityFrames;

        writer_ = std::make_unique<cleanmic::ipc::SharedAudioPipeWriter>(std::move(config));
        opened_ = writer_->open();
        if (!opened_) {
            diagnostic_ = writer_->diagnostic();
        } else {
            diagnostic_ = "Shared CleanMic feed is live on " + writer_->name();
        }
        return opened_;
    }

    bool write(const float* samples, std::size_t frame_count) override {
        return opened_ && writer_ != nullptr && writer_->write_interleaved(samples, frame_count);
    }

    void close() override {
        if (writer_ != nullptr) {
            writer_->close(false);
        }
        opened_ = false;
    }

    std::string diagnostic() const override {
        if (!diagnostic_.empty()) {
            return diagnostic_;
        }
        return "Shared CleanMic feed is available, but the HAL bundle still needs to be installed "
               "into /Library/Audio/Plug-Ins/HAL.";
    }

private:
    std::unique_ptr<cleanmic::ipc::SharedAudioPipeWriter> writer_;
    bool opened_ = false;
    std::string diagnostic_;
};

#else

class NullVirtualMicrophone final : public VirtualMicrophone {
public:
    bool open(int sample_rate, int channels) override {
        sample_rate_ = sample_rate;
        channels_ = channels;
        opened_ = false;
        return false;
    }

    bool write(const float* samples, std::size_t frame_count) override {
        (void)samples;
        (void)frame_count;
        return false;
    }

    void close() override {
        opened_ = false;
    }

    std::string diagnostic() const override {
#if defined(__APPLE__)
        return "No macOS virtual microphone backend compiled. Add a Core Audio / AudioDriverKit "
               "adapter and feed it from cleanmic_core.";
#elif defined(_WIN32)
        return "No Windows virtual microphone backend compiled. Add a virtual audio device "
               "backend and connect it to cleanmic_core.";
#else
        return "No Linux virtual microphone backend compiled. Add a PipeWire virtual source "
               "backend and connect it to cleanmic_core.";
#endif
    }

private:
    int sample_rate_ = 0;
    int channels_ = 0;
    bool opened_ = false;
};

#endif

}  // namespace

std::unique_ptr<VirtualMicrophone> create_virtual_microphone() {
#if defined(__APPLE__)
    return std::make_unique<SharedPipeVirtualMicrophone>();
#else
    return std::make_unique<NullVirtualMicrophone>();
#endif
}

}  // namespace cleanmic::platform
