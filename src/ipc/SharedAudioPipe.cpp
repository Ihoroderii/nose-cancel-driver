#include "cleanmic/ipc/SharedAudioPipe.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#error "SharedAudioPipe is currently implemented only for POSIX platforms."
#endif

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace cleanmic::ipc {
namespace {

std::uint64_t monotonic_time_ns() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

std::size_t total_mapping_size(std::uint32_t capacity_frames, std::uint32_t channels) {
    return sizeof(SharedAudioPipeHeader) +
           static_cast<std::size_t>(capacity_frames) * static_cast<std::size_t>(channels) *
               sizeof(float);
}

float* ring_samples(SharedAudioPipeHeader* header) {
    return reinterpret_cast<float*>(reinterpret_cast<unsigned char*>(header) +
                                    sizeof(SharedAudioPipeHeader));
}

const float* ring_samples(const SharedAudioPipeHeader* header) {
    return reinterpret_cast<const float*>(reinterpret_cast<const unsigned char*>(header) +
                                          sizeof(SharedAudioPipeHeader));
}

std::string errno_string(int err) {
    return std::generic_category().message(err);
}

struct SharedMapping {
    int fd = -1;
    std::size_t size = 0;
    SharedAudioPipeHeader* header = nullptr;

    void close_mapping() {
        if (header != nullptr) {
            munmap(header, size);
            header = nullptr;
        }
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
        size = 0;
    }
};

bool is_layout_compatible(const SharedAudioPipeHeader* header, const SharedAudioPipeConfig& config) {
    return header != nullptr && header->magic == kSharedAudioPipeMagic &&
           header->version == kSharedAudioPipeVersion &&
           header->sample_rate == config.sample_rate && header->channels == config.channels &&
           header->capacity_frames == config.capacity_frames;
}

void initialize_layout(SharedAudioPipeHeader* header, const SharedAudioPipeConfig& config) {
    std::memset(header, 0, total_mapping_size(config.capacity_frames, config.channels));
    header->magic = kSharedAudioPipeMagic;
    header->version = kSharedAudioPipeVersion;
    header->sample_rate = config.sample_rate;
    header->channels = config.channels;
    header->capacity_frames = config.capacity_frames;
    header->write_index.store(0, std::memory_order_relaxed);
    header->read_index.store(0, std::memory_order_relaxed);
    header->overrun_count.store(0, std::memory_order_relaxed);
    header->stream_epoch.store(1, std::memory_order_relaxed);
    header->last_write_host_time_ns.store(0, std::memory_order_relaxed);
    header->active.store(0, std::memory_order_relaxed);
    header->writer_pid.store(0, std::memory_order_relaxed);
}

SharedMapping open_mapping(const std::string& name, std::uint32_t sample_rate, std::uint32_t channels,
                           std::uint32_t capacity_frames, bool create) {
    SharedAudioPipeConfig config;
    config.name = name;
    config.sample_rate = sample_rate;
    config.channels = channels;
    config.capacity_frames = capacity_frames;

    const int flags = create ? (O_RDWR | O_CREAT) : O_RDWR;
    const int fd = shm_open(config.name.c_str(), flags, 0600);
    if (fd < 0) {
        return {};
    }

    SharedMapping mapping;
    mapping.fd = fd;
    mapping.size = total_mapping_size(config.capacity_frames, config.channels);

    if (create) {
        if (ftruncate(fd, static_cast<off_t>(mapping.size)) != 0) {
            mapping.close_mapping();
            return {};
        }
    }

    void* base = mmap(nullptr, mapping.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        mapping.close_mapping();
        return {};
    }

    mapping.header = static_cast<SharedAudioPipeHeader*>(base);
    if (create && !is_layout_compatible(mapping.header, config)) {
        initialize_layout(mapping.header, config);
    }
    return mapping;
}

std::size_t sample_offset(const SharedAudioPipeHeader* header, std::uint64_t frame_index,
                          std::uint32_t channel) {
    const std::size_t frame_slot = static_cast<std::size_t>(frame_index % header->capacity_frames);
    return frame_slot * static_cast<std::size_t>(header->channels) + channel;
}

}  // namespace

struct SharedAudioPipeWriter::Impl {
    SharedMapping mapping;
    std::string diagnostic;
};

struct SharedAudioPipeReader::Impl {
    SharedMapping mapping;
    std::string diagnostic;
};

SharedAudioPipeWriter::SharedAudioPipeWriter(SharedAudioPipeConfig config)
    : config_(std::move(config)), impl_(new Impl()) {}

SharedAudioPipeWriter::~SharedAudioPipeWriter() {
    close(false);
    delete impl_;
    impl_ = nullptr;
}

SharedAudioPipeWriter::SharedAudioPipeWriter(SharedAudioPipeWriter&& other) noexcept
    : config_(std::move(other.config_)), impl_(other.impl_) {
    other.impl_ = nullptr;
}

SharedAudioPipeWriter& SharedAudioPipeWriter::operator=(SharedAudioPipeWriter&& other) noexcept {
    if (this != &other) {
        close(false);
        delete impl_;
        config_ = std::move(other.config_);
        impl_ = other.impl_;
        other.impl_ = nullptr;
    }
    return *this;
}

bool SharedAudioPipeWriter::open() {
    if (impl_ == nullptr) {
        return false;
    }
    close(false);

    if (config_.channels == 0 || config_.capacity_frames == 0 || config_.sample_rate == 0) {
        impl_->diagnostic = "Invalid shared audio pipe configuration.";
        return false;
    }

    impl_->mapping = open_mapping(config_.name, config_.sample_rate, config_.channels,
                                  config_.capacity_frames, true);
    if (impl_->mapping.header == nullptr) {
        impl_->diagnostic = "Failed to open shared memory " + config_.name + ": " +
                            errno_string(errno);
        return false;
    }

    impl_->mapping.header->active.store(1, std::memory_order_release);
    impl_->mapping.header->writer_pid.store(static_cast<std::uint32_t>(::getpid()),
                                            std::memory_order_release);
    impl_->mapping.header->stream_epoch.fetch_add(1, std::memory_order_acq_rel);
    impl_->diagnostic = "Shared audio pipe writer ready on " + config_.name;
    return true;
}

void SharedAudioPipeWriter::close(bool unlink_segment) {
    if (impl_ == nullptr) {
        return;
    }
    if (impl_->mapping.header != nullptr) {
        impl_->mapping.header->active.store(0, std::memory_order_release);
        impl_->mapping.header->writer_pid.store(0, std::memory_order_release);
    }
    impl_->mapping.close_mapping();
    if (unlink_segment) {
        shm_unlink(config_.name.c_str());
    }
}

bool SharedAudioPipeWriter::is_open() const {
    return impl_ != nullptr && impl_->mapping.header != nullptr;
}

bool SharedAudioPipeWriter::write_interleaved(const float* samples, std::size_t frame_count) {
    if (!is_open() || samples == nullptr || frame_count == 0) {
        return false;
    }

    SharedAudioPipeHeader* header = impl_->mapping.header;
    float* ring = ring_samples(header);

    std::uint64_t read_index = header->read_index.load(std::memory_order_acquire);
    std::uint64_t write_index = header->write_index.load(std::memory_order_relaxed);
    const std::uint64_t used = write_index - read_index;
    const std::uint64_t capacity = header->capacity_frames;

    if (frame_count >= capacity) {
        samples += (frame_count - capacity) * header->channels;
        frame_count = static_cast<std::size_t>(capacity);
        read_index = write_index;
    } else {
        const std::uint64_t free_frames = capacity > used ? capacity - used : 0;
        if (frame_count > free_frames) {
            const std::uint64_t drop_frames = frame_count - free_frames;
            header->read_index.store(read_index + drop_frames, std::memory_order_release);
            header->overrun_count.fetch_add(1, std::memory_order_acq_rel);
            read_index += drop_frames;
        }
    }

    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        for (std::uint32_t channel = 0; channel < header->channels; ++channel) {
            const std::size_t source_index =
                frame * static_cast<std::size_t>(header->channels) + channel;
            ring[sample_offset(header, write_index + frame, channel)] = samples[source_index];
        }
    }

    header->last_write_host_time_ns.store(monotonic_time_ns(), std::memory_order_release);
    header->write_index.store(write_index + frame_count, std::memory_order_release);
    return true;
}

std::uint32_t SharedAudioPipeWriter::sample_rate() const {
    return config_.sample_rate;
}

std::uint32_t SharedAudioPipeWriter::channels() const {
    return config_.channels;
}

std::uint32_t SharedAudioPipeWriter::capacity_frames() const {
    return config_.capacity_frames;
}

const std::string& SharedAudioPipeWriter::name() const {
    return config_.name;
}

std::string SharedAudioPipeWriter::diagnostic() const {
    return impl_ != nullptr ? impl_->diagnostic : "Shared audio pipe writer is unavailable.";
}

SharedAudioPipeReader::SharedAudioPipeReader(std::string name) : name_(std::move(name)), impl_(new Impl()) {}

SharedAudioPipeReader::~SharedAudioPipeReader() {
    close();
    delete impl_;
    impl_ = nullptr;
}

SharedAudioPipeReader::SharedAudioPipeReader(SharedAudioPipeReader&& other) noexcept
    : name_(std::move(other.name_)), impl_(other.impl_) {
    other.impl_ = nullptr;
}

SharedAudioPipeReader& SharedAudioPipeReader::operator=(SharedAudioPipeReader&& other) noexcept {
    if (this != &other) {
        close();
        delete impl_;
        name_ = std::move(other.name_);
        impl_ = other.impl_;
        other.impl_ = nullptr;
    }
    return *this;
}

bool SharedAudioPipeReader::open() {
    if (impl_ == nullptr) {
        return false;
    }
    close();

    impl_->mapping = open_mapping(name_, 48000, 1, kDefaultSharedAudioPipeCapacityFrames, false);
    if (impl_->mapping.header == nullptr) {
        impl_->diagnostic = "Shared audio pipe is not available on " + name_;
        return false;
    }

    if (impl_->mapping.header->magic != kSharedAudioPipeMagic ||
        impl_->mapping.header->version != kSharedAudioPipeVersion) {
        impl_->diagnostic = "Shared audio pipe " + name_ + " has an incompatible layout.";
        close();
        return false;
    }

    impl_->diagnostic = "Shared audio pipe reader attached to " + name_;
    return true;
}

void SharedAudioPipeReader::close() {
    if (impl_ != nullptr) {
        impl_->mapping.close_mapping();
    }
}

bool SharedAudioPipeReader::is_open() const {
    return impl_ != nullptr && impl_->mapping.header != nullptr;
}

std::size_t SharedAudioPipeReader::available_frames() const {
    if (!is_open()) {
        return 0;
    }
    const SharedAudioPipeHeader* header = impl_->mapping.header;
    const std::uint64_t read_index = header->read_index.load(std::memory_order_acquire);
    const std::uint64_t write_index = header->write_index.load(std::memory_order_acquire);
    return static_cast<std::size_t>(write_index - read_index);
}

std::size_t SharedAudioPipeReader::read_interleaved(float* output, std::size_t frame_count,
                                                    bool zero_fill) {
    if (output == nullptr || frame_count == 0) {
        return 0;
    }

    if (!is_open()) {
        if (zero_fill) {
            std::fill(output, output + frame_count, 0.0f);
        }
        return 0;
    }

    SharedAudioPipeHeader* header = impl_->mapping.header;
    const float* ring = ring_samples(header);
    const std::uint64_t read_index = header->read_index.load(std::memory_order_relaxed);
    const std::uint64_t write_index = header->write_index.load(std::memory_order_acquire);
    const std::size_t available =
        static_cast<std::size_t>(std::max<std::uint64_t>(0, write_index - read_index));
    const std::size_t to_copy = std::min(frame_count, available);

    for (std::size_t frame = 0; frame < to_copy; ++frame) {
        for (std::uint32_t channel = 0; channel < header->channels; ++channel) {
            output[frame * static_cast<std::size_t>(header->channels) + channel] =
                ring[sample_offset(header, read_index + frame, channel)];
        }
    }

    if (zero_fill && to_copy < frame_count) {
        std::fill(output + to_copy * static_cast<std::size_t>(header->channels),
                  output + frame_count * static_cast<std::size_t>(header->channels), 0.0f);
    }

    header->read_index.store(read_index + to_copy, std::memory_order_release);
    return to_copy;
}

std::uint32_t SharedAudioPipeReader::sample_rate() const {
    return is_open() ? impl_->mapping.header->sample_rate : 0;
}

std::uint32_t SharedAudioPipeReader::channels() const {
    return is_open() ? impl_->mapping.header->channels : 0;
}

std::uint32_t SharedAudioPipeReader::capacity_frames() const {
    return is_open() ? impl_->mapping.header->capacity_frames : 0;
}

bool SharedAudioPipeReader::active() const {
    return is_open() && impl_->mapping.header->active.load(std::memory_order_acquire) != 0;
}

std::uint64_t SharedAudioPipeReader::stream_epoch() const {
    return is_open() ? impl_->mapping.header->stream_epoch.load(std::memory_order_acquire) : 0;
}

std::string SharedAudioPipeReader::diagnostic() const {
    return impl_ != nullptr ? impl_->diagnostic : "Shared audio pipe reader is unavailable.";
}

}  // namespace cleanmic::ipc
