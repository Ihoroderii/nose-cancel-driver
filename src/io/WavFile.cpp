#include "cleanmic/io/WavFile.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace cleanmic::io {
namespace {

constexpr float kInt16Scale = 32768.0f;
constexpr std::uint16_t kWaveFormatPcm = 0x0001;
constexpr std::uint16_t kWaveFormatFloat = 0x0003;
constexpr std::uint16_t kWaveFormatExtensible = 0xFFFE;

std::uint16_t read_u16(std::istream& stream) {
    unsigned char bytes[2]{};
    stream.read(reinterpret_cast<char*>(bytes), sizeof(bytes));
    if (!stream) {
        throw std::runtime_error("Unexpected end of file while reading 16-bit value.");
    }
    return static_cast<std::uint16_t>(bytes[0] | (bytes[1] << 8));
}

std::uint32_t read_u32(std::istream& stream) {
    unsigned char bytes[4]{};
    stream.read(reinterpret_cast<char*>(bytes), sizeof(bytes));
    if (!stream) {
        throw std::runtime_error("Unexpected end of file while reading 32-bit value.");
    }
    return static_cast<std::uint32_t>(bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) |
                                      (bytes[3] << 24));
}

void write_u16(std::ostream& stream, std::uint16_t value) {
    const unsigned char bytes[2] = {
        static_cast<unsigned char>(value & 0xFFu),
        static_cast<unsigned char>((value >> 8) & 0xFFu),
    };
    stream.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
}

void write_u32(std::ostream& stream, std::uint32_t value) {
    const unsigned char bytes[4] = {
        static_cast<unsigned char>(value & 0xFFu),
        static_cast<unsigned char>((value >> 8) & 0xFFu),
        static_cast<unsigned char>((value >> 16) & 0xFFu),
        static_cast<unsigned char>((value >> 24) & 0xFFu),
    };
    stream.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
}

std::string read_tag(std::istream& stream) {
    char tag[4]{};
    stream.read(tag, sizeof(tag));
    if (!stream) {
        throw std::runtime_error("Unexpected end of file while reading chunk tag.");
    }
    return std::string(tag, sizeof(tag));
}

void skip_padding(std::istream& stream, std::uint32_t chunk_size) {
    if ((chunk_size & 1u) != 0u) {
        stream.ignore(1);
    }
}

}  // namespace

WavData read_wav_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Failed to open WAV file: " + path);
    }

    if (read_tag(file) != "RIFF") {
        throw std::runtime_error("Invalid WAV file: missing RIFF header.");
    }
    (void)read_u32(file);
    if (read_tag(file) != "WAVE") {
        throw std::runtime_error("Invalid WAV file: missing WAVE header.");
    }

    bool have_fmt = false;
    bool have_data = false;
    std::uint16_t audio_format = 0;
    std::uint16_t decoded_format = 0;
    std::uint16_t channels = 0;
    std::uint32_t sample_rate = 0;
    std::uint16_t bits_per_sample = 0;
    std::vector<char> raw_data;

    while (file.peek() != std::char_traits<char>::eof()) {
        const std::string chunk_id = read_tag(file);
        const std::uint32_t chunk_size = read_u32(file);

        if (chunk_id == "fmt ") {
            audio_format = read_u16(file);
            decoded_format = audio_format;
            channels = read_u16(file);
            sample_rate = read_u32(file);
            (void)read_u32(file);
            (void)read_u16(file);
            bits_per_sample = read_u16(file);

            if (chunk_size > 16) {
                const std::uint16_t extension_size = read_u16(file);
                std::uint32_t bytes_consumed = 18;

                if (audio_format == kWaveFormatExtensible && extension_size >= 22 &&
                    chunk_size >= 40) {
                    (void)read_u16(file);   // valid bits per sample
                    (void)read_u32(file);   // channel mask
                    decoded_format = read_u16(file);
                    file.ignore(14);        // remaining GUID bytes
                    bytes_consumed += 22;
                } else {
                    const std::streamsize to_skip =
                        static_cast<std::streamsize>(std::min<std::uint32_t>(
                            extension_size, chunk_size > bytes_consumed ? chunk_size - bytes_consumed : 0));
                    file.ignore(to_skip);
                    bytes_consumed += static_cast<std::uint32_t>(to_skip);
                }

                if (chunk_size > bytes_consumed) {
                    file.ignore(static_cast<std::streamsize>(chunk_size - bytes_consumed));
                }
            }
            have_fmt = true;
        } else if (chunk_id == "data") {
            raw_data.resize(chunk_size);
            file.read(raw_data.data(), static_cast<std::streamsize>(chunk_size));
            if (!file) {
                throw std::runtime_error("Failed while reading WAV data chunk.");
            }
            have_data = true;
        } else {
            file.ignore(static_cast<std::streamsize>(chunk_size));
        }

        skip_padding(file, chunk_size);
    }

    if (!have_fmt || !have_data) {
        throw std::runtime_error("WAV file is missing fmt or data chunk.");
    }

    WavData wav;
    wav.sample_rate = static_cast<int>(sample_rate);
    wav.channels = static_cast<int>(channels);

    if (decoded_format == kWaveFormatPcm && bits_per_sample == 16) {
        const std::size_t sample_count = raw_data.size() / sizeof(std::int16_t);
        wav.samples.resize(sample_count);
        for (std::size_t i = 0; i < sample_count; ++i) {
            std::int16_t sample = 0;
            std::memcpy(&sample, raw_data.data() + i * sizeof(std::int16_t), sizeof(sample));
            wav.samples[i] = static_cast<float>(sample) / kInt16Scale;
        }
    } else if (decoded_format == kWaveFormatFloat && bits_per_sample == 32) {
        const std::size_t sample_count = raw_data.size() / sizeof(float);
        wav.samples.resize(sample_count);
        for (std::size_t i = 0; i < sample_count; ++i) {
            float sample = 0.0f;
            std::memcpy(&sample, raw_data.data() + i * sizeof(float), sizeof(sample));
            wav.samples[i] = sample;
        }
    } else {
        throw std::runtime_error("Unsupported WAV format. Use PCM16 or float32 WAV files.");
    }

    return wav;
}

void write_wav_file(const std::string& path, const WavData& wav) {
    if (wav.channels <= 0 || wav.sample_rate <= 0) {
        throw std::runtime_error("Invalid WAV metadata.");
    }

    std::ofstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Failed to open output WAV file: " + path);
    }

    const std::uint16_t channels = static_cast<std::uint16_t>(wav.channels);
    const std::uint16_t bits_per_sample = 16;
    const std::uint16_t block_align = static_cast<std::uint16_t>(channels * bits_per_sample / 8);
    const std::uint32_t byte_rate =
        static_cast<std::uint32_t>(wav.sample_rate) * static_cast<std::uint32_t>(block_align);
    const std::uint32_t data_size =
        static_cast<std::uint32_t>(wav.samples.size() * sizeof(std::int16_t));
    const std::uint32_t riff_size = 4 + 8 + 16 + 8 + data_size;

    file.write("RIFF", 4);
    write_u32(file, riff_size);
    file.write("WAVE", 4);

    file.write("fmt ", 4);
    write_u32(file, 16);
    write_u16(file, 1);
    write_u16(file, channels);
    write_u32(file, static_cast<std::uint32_t>(wav.sample_rate));
    write_u32(file, byte_rate);
    write_u16(file, block_align);
    write_u16(file, bits_per_sample);

    file.write("data", 4);
    write_u32(file, data_size);

    for (float sample : wav.samples) {
        const float clamped = std::clamp(sample, -1.0f, 1.0f);
        const auto pcm = static_cast<std::int16_t>(clamped * 32767.0f);
        file.write(reinterpret_cast<const char*>(&pcm), sizeof(pcm));
    }
}

std::vector<float> to_mono(const WavData& wav) {
    if (wav.channels <= 0) {
        return {};
    }

    if (wav.channels == 1) {
        return wav.samples;
    }

    const std::size_t frame_count = wav.samples.size() / static_cast<std::size_t>(wav.channels);
    std::vector<float> mono(frame_count, 0.0f);

    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        float accum = 0.0f;
        for (int channel = 0; channel < wav.channels; ++channel) {
            accum += wav.samples[frame * static_cast<std::size_t>(wav.channels) +
                                 static_cast<std::size_t>(channel)];
        }
        mono[frame] = accum / static_cast<float>(wav.channels);
    }

    return mono;
}

}  // namespace cleanmic::io
