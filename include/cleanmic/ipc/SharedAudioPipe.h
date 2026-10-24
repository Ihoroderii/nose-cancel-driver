#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace cleanmic::ipc {

constexpr const char* kDefaultSharedAudioPipeName = "/cleanmic.virtual_mic";
constexpr std::uint32_t kSharedAudioPipeMagic = 0x434D5049;  // CMPI
constexpr std::uint32_t kSharedAudioPipeVersion = 1;
constexpr std::uint32_t kDefaultSharedAudioPipeCapacityFrames = 48000 * 8;

struct SharedAudioPipeConfig {
    std::string name = kDefaultSharedAudioPipeName;
    std::uint32_t sample_rate = 48000;
    std::uint32_t channels = 1;
    std::uint32_t capacity_frames = kDefaultSharedAudioPipeCapacityFrames;
};

struct alignas(64) SharedAudioPipeHeader {
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::uint32_t sample_rate = 0;
    std::uint32_t channels = 0;
    std::uint32_t capacity_frames = 0;
    std::uint32_t reserved0 = 0;
    std::atomic<std::uint64_t> write_index{0};
    std::atomic<std::uint64_t> read_index{0};
    std::atomic<std::uint64_t> overrun_count{0};
    std::atomic<std::uint64_t> stream_epoch{0};
    std::atomic<std::uint64_t> last_write_host_time_ns{0};
    std::atomic<std::uint32_t> active{0};
    std::atomic<std::uint32_t> writer_pid{0};
};

class SharedAudioPipeWriter {
public:
    SharedAudioPipeWriter() = default;
    explicit SharedAudioPipeWriter(SharedAudioPipeConfig config);
    ~SharedAudioPipeWriter();

    SharedAudioPipeWriter(const SharedAudioPipeWriter&) = delete;
    SharedAudioPipeWriter& operator=(const SharedAudioPipeWriter&) = delete;

    SharedAudioPipeWriter(SharedAudioPipeWriter&& other) noexcept;
    SharedAudioPipeWriter& operator=(SharedAudioPipeWriter&& other) noexcept;

    bool open();
    void close(bool unlink_segment = false);
    bool is_open() const;

    bool write_interleaved(const float* samples, std::size_t frame_count);

    std::uint32_t sample_rate() const;
    std::uint32_t channels() const;
    std::uint32_t capacity_frames() const;
    const std::string& name() const;
    std::string diagnostic() const;

private:
    struct Impl;
    SharedAudioPipeConfig config_;
    Impl* impl_ = nullptr;
};

class SharedAudioPipeReader {
public:
    SharedAudioPipeReader() = default;
    explicit SharedAudioPipeReader(std::string name);
    ~SharedAudioPipeReader();

    SharedAudioPipeReader(const SharedAudioPipeReader&) = delete;
    SharedAudioPipeReader& operator=(const SharedAudioPipeReader&) = delete;

    SharedAudioPipeReader(SharedAudioPipeReader&& other) noexcept;
    SharedAudioPipeReader& operator=(SharedAudioPipeReader&& other) noexcept;

    bool open();
    void close();
    bool is_open() const;

    std::size_t available_frames() const;
    std::size_t read_interleaved(float* output, std::size_t frame_count, bool zero_fill = true);

    std::uint32_t sample_rate() const;
    std::uint32_t channels() const;
    std::uint32_t capacity_frames() const;
    bool active() const;
    std::uint64_t stream_epoch() const;
    std::string diagnostic() const;

private:
    struct Impl;
    std::string name_ = kDefaultSharedAudioPipeName;
    Impl* impl_ = nullptr;
};

}  // namespace cleanmic::ipc
