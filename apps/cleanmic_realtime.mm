#import <AVFoundation/AVFoundation.h>

#include "cleanmic/io/WavFile.h"
#include "cleanmic/realtime/SpscFloatRingBuffer.h"
#include "cleanmic/realtime/StreamingProcessor.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cctype>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> g_should_stop{false};

void handle_signal(int) {
    g_should_stop.store(true);
}

enum class LiveMode {
    hush,
    rnnoise,
    bypass,
};

struct Options {
    LiveMode mode = LiveMode::hush;
    float hush_attenuation_limit_db = 100.0f;
    float input_gain_db = 0.0f;
    float output_gain_db = 0.0f;
    bool monitor_output = false;
    std::string save_prefix = "capture";
    bool save_files = true;
};

float db_to_gain(float db) {
    return std::pow(10.0f, db / 20.0f);
}

float clamp_sample(float sample) {
    return std::max(-1.0f, std::min(1.0f, sample));
}

std::string to_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

LiveMode parse_mode(const std::string& value) {
    const std::string lowered = to_lower(value);
    if (lowered == "hush") {
        return LiveMode::hush;
    }
    if (lowered == "rnnoise") {
        return LiveMode::rnnoise;
    }
    if (lowered == "bypass") {
        return LiveMode::bypass;
    }

    throw std::runtime_error("Unknown live mode: " + value + ". Use hush, rnnoise, or bypass.");
}

cleanmic::realtime::LiveBackend to_backend(LiveMode mode) {
    switch (mode) {
        case LiveMode::hush:
            return cleanmic::realtime::LiveBackend::hush;
        case LiveMode::rnnoise:
            return cleanmic::realtime::LiveBackend::rnnoise;
        case LiveMode::bypass:
            return cleanmic::realtime::LiveBackend::bypass;
    }
    return cleanmic::realtime::LiveBackend::hush;
}

std::string backend_name(LiveMode mode) {
    switch (mode) {
        case LiveMode::hush:
            return "hush";
        case LiveMode::rnnoise:
            return "rnnoise";
        case LiveMode::bypass:
            return "bypass";
    }
    return "unknown";
}

void print_usage() {
    std::cout << "Usage:\n"
              << "  cleanmic_realtime [--mode hush|rnnoise|bypass] [--save-prefix capture]\n"
              << "  cleanmic_realtime [--mode hush|rnnoise|bypass] [--no-save]\n"
              << "  cleanmic_realtime [--monitor] [--input-gain-db 0] [--output-gain-db 0]\n";
}

Options parse_options(int argc, char** argv) {
    Options options;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--mode" && i + 1 < argc) {
            options.mode = parse_mode(argv[++i]);
        } else if (arg == "--save-prefix" && i + 1 < argc) {
            options.save_prefix = argv[++i];
            options.save_files = true;
        } else if (arg == "--no-save") {
            options.save_files = false;
        } else if (arg == "--atten-lim-db" && i + 1 < argc) {
            options.hush_attenuation_limit_db = std::stof(argv[++i]);
        } else if (arg == "--input-gain-db" && i + 1 < argc) {
            options.input_gain_db = std::stof(argv[++i]);
        } else if (arg == "--output-gain-db" && i + 1 < argc) {
            options.output_gain_db = std::stof(argv[++i]);
        } else if (arg == "--monitor") {
            options.monitor_output = true;
        } else if (arg == "--no-monitor") {
            options.monitor_output = false;
        } else if (arg == "--help" || arg == "-h") {
            print_usage();
            std::exit(0);
        } else {
            throw std::runtime_error("Unknown or incomplete argument: " + arg);
        }
    }

    return options;
}

bool request_microphone_access() {
    AVAuthorizationStatus status =
        [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];

    if (status == AVAuthorizationStatusAuthorized) {
        return true;
    }
    if (status == AVAuthorizationStatusDenied || status == AVAuthorizationStatusRestricted) {
        return false;
    }

    __block bool granted = false;
    dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
    [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                             completionHandler:^(BOOL accessGranted) {
                                 granted = accessGranted;
                                 dispatch_semaphore_signal(semaphore);
                             }];
    dispatch_semaphore_wait(semaphore, DISPATCH_TIME_FOREVER);
    return granted;
}

std::string add_suffix_to_path(const std::string& path, const std::string& suffix) {
    const std::size_t last_separator = path.find_last_of("/\\");
    const std::size_t last_dot = path.find_last_of('.');
    if (last_dot == std::string::npos ||
        (last_separator != std::string::npos && last_dot < last_separator)) {
        return path + suffix + ".wav";
    }

    return path.substr(0, last_dot) + suffix + path.substr(last_dot);
}

}  // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        try {
            const Options options = parse_options(argc, argv);

            if (!request_microphone_access()) {
                std::cerr << "Microphone access was denied. Allow Terminal microphone access in "
                             "System Settings and try again.\n";
                return 1;
            }

            std::signal(SIGINT, handle_signal);
            std::signal(SIGTERM, handle_signal);

            AVAudioEngine* engine = [[AVAudioEngine alloc] init];
            AVAudioInputNode* input_node = [engine inputNode];
            if (input_node == nil) {
                throw std::runtime_error("No audio input node is available.");
            }
            engine.mainMixerNode.outputVolume = options.monitor_output ? 1.0f : 0.0f;

            AVAudioFormat* hardware_input_format = [input_node inputFormatForBus:0];
            if (hardware_input_format == nil || hardware_input_format.sampleRate <= 0.0 ||
                hardware_input_format.channelCount == 0) {
                throw std::runtime_error("Failed to query the input audio format.");
            }

            const int sample_rate = static_cast<int>(std::llround(hardware_input_format.sampleRate));
            const AVAudioChannelCount input_channels = hardware_input_format.channelCount;

            auto processor = std::make_shared<cleanmic::realtime::StreamingProcessor>(
                to_backend(options.mode), sample_rate, options.hush_attenuation_limit_db);
            const std::size_t frame_size = processor->frame_size();
            std::shared_ptr<cleanmic::realtime::SpscFloatRingBuffer> ring_buffer;
            std::shared_ptr<std::atomic<bool>> playback_started;
            std::size_t startup_buffer = 0;
            const float input_gain = db_to_gain(options.input_gain_db);
            const float output_gain = db_to_gain(options.output_gain_db);

            if (options.monitor_output) {
                ring_buffer = std::make_shared<cleanmic::realtime::SpscFloatRingBuffer>(
                    std::max<std::size_t>(frame_size * 64, static_cast<std::size_t>(sample_rate)));
                playback_started = std::make_shared<std::atomic<bool>>(false);
                startup_buffer = frame_size * 2;
            }

            AVAudioFormat* tap_format =
                [[AVAudioFormat alloc] initStandardFormatWithSampleRate:sample_rate
                                                               channels:input_channels];
            AVAudioFormat* playback_format = nil;
            if (options.monitor_output) {
                playback_format =
                    [[AVAudioFormat alloc] initStandardFormatWithSampleRate:sample_rate channels:2];
            }

            auto raw_capture = std::make_shared<std::vector<float>>();
            auto processed_capture = std::make_shared<std::vector<float>>();
            auto capture_mutex = std::make_shared<std::mutex>();

            AVAudioSourceNode* source_node = nil;
            if (options.monitor_output) {
                source_node = [[AVAudioSourceNode alloc]
                    initWithRenderBlock:^OSStatus(BOOL* isSilence,
                                                  const AudioTimeStamp*,
                                                  AVAudioFrameCount frameCount,
                                                  AudioBufferList* outputData) {
                        std::vector<float> mono(frameCount, 0.0f);
                        if (!playback_started->load(std::memory_order_acquire)) {
                            if (ring_buffer->available() >= startup_buffer) {
                                playback_started->store(true, std::memory_order_release);
                            }
                        }

                        if (playback_started->load(std::memory_order_acquire)) {
                            ring_buffer->pop(mono.data(), frameCount);
                            for (float& sample : mono) {
                                sample = clamp_sample(sample * output_gain);
                            }
                        }

                        for (UInt32 buffer_index = 0; buffer_index < outputData->mNumberBuffers;
                             ++buffer_index) {
                            AudioBuffer& buffer = outputData->mBuffers[buffer_index];
                            float* destination = static_cast<float*>(buffer.mData);
                            for (AVAudioFrameCount frame = 0; frame < frameCount; ++frame) {
                                destination[frame] = mono[frame];
                            }
                        }

                        if (isSilence != nullptr) {
                            *isSilence = NO;
                        }
                        return noErr;
                    }];

                [engine attachNode:source_node];
                [engine connect:source_node to:engine.mainMixerNode format:playback_format];
            }

            [input_node installTapOnBus:0
                              bufferSize:static_cast<AVAudioFrameCount>(frame_size)
                                  format:tap_format
                                   block:^(AVAudioPCMBuffer* buffer, AVAudioTime*) {
                                       const AVAudioFrameCount count = buffer.frameLength;
                                       float* const* channels = buffer.floatChannelData;
                                       if (count == 0 || channels == nullptr) {
                                           return;
                                       }

                                       std::vector<float> raw_mono(count, 0.0f);
                                       std::vector<float> mono(count, 0.0f);
                                       for (AVAudioFrameCount frame = 0; frame < count; ++frame) {
                                           const float captured = clamp_sample(channels[0][frame]);
                                           raw_mono[frame] = captured;
                                           mono[frame] = clamp_sample(captured * input_gain);
                                       }

                                       processor->push_input(mono.data(), count);
                                       const std::size_t available = processor->available_output();
                                       if (available > 0) {
                                           std::vector<float> processed(available, 0.0f);
                                           const std::size_t produced =
                                               processor->pop_output(processed.data(), available, false);
                                           processed.resize(produced);
                                           if (options.monitor_output && !processed.empty()) {
                                               ring_buffer->push(processed.data(), processed.size());
                                           }

                                           if (options.save_files) {
                                               std::lock_guard<std::mutex> lock(*capture_mutex);
                                               processed_capture->insert(processed_capture->end(),
                                                                         processed.begin(),
                                                                         processed.end());
                                           }
                                       }

                                       if (options.save_files) {
                                           std::lock_guard<std::mutex> lock(*capture_mutex);
                                           raw_capture->insert(raw_capture->end(),
                                                               raw_mono.begin(),
                                                               raw_mono.end());
                                       }
                                   }];

            [engine prepare];
            NSError* error = nil;
            if (![engine startAndReturnError:&error]) {
                const std::string reason =
                    (error != nil) ? std::string([[error localizedDescription] UTF8String])
                                   : "unknown AVAudioEngine error";
                throw std::runtime_error("Failed to start AVAudioEngine: " + reason);
            }

            std::cout << "Live backend: " << backend_name(options.mode) << '\n';
            std::cout << "Input sample rate: " << sample_rate << " Hz\n";
            std::cout << "Input channels: " << input_channels << '\n';
            if (input_channels > 1) {
                std::cout << "Capture mix: first input channel only\n";
            }
            std::cout << "Processing frame size: " << frame_size << " samples\n";
            std::cout << "Input gain: " << options.input_gain_db << " dB\n";
            std::cout << "Speaker monitor: " << (options.monitor_output ? "enabled" : "disabled")
                      << '\n';
            if (options.monitor_output) {
                std::cout << "Output gain: " << options.output_gain_db << " dB\n";
                std::cout << "Use headphones to avoid feedback. Press Ctrl+C to stop.\n";
            } else {
                std::cout << "Playback is off, so the saved files are the clean comparison path.\n";
                std::cout << "Press Ctrl+C to stop.\n";
            }
            if (options.save_files) {
                std::cout << "Will save raw and processed captures with prefix: "
                          << options.save_prefix << '\n';
            }

            while (!g_should_stop.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            [input_node removeTapOnBus:0];
            [engine stop];

            if (options.save_files) {
                std::lock_guard<std::mutex> lock(*capture_mutex);

                cleanmic::io::WavData raw_wav;
                raw_wav.sample_rate = sample_rate;
                raw_wav.channels = 1;
                raw_wav.samples = *raw_capture;
                cleanmic::io::write_wav_file(add_suffix_to_path(options.save_prefix, "_raw"), raw_wav);

                cleanmic::io::WavData processed_wav;
                processed_wav.sample_rate = sample_rate;
                processed_wav.channels = 1;
                processed_wav.samples = *processed_capture;
                cleanmic::io::write_wav_file(
                    add_suffix_to_path(options.save_prefix, "_" + backend_name(options.mode)),
                    processed_wav);

                std::cout << "Saved raw capture to "
                          << add_suffix_to_path(options.save_prefix, "_raw") << '\n';
                std::cout << "Saved processed capture to "
                          << add_suffix_to_path(options.save_prefix,
                                               "_" + backend_name(options.mode))
                          << '\n';
            }

            return 0;
        } catch (const std::exception& exception) {
            std::cerr << "Error: " << exception.what() << '\n';
            return 1;
        }
    }
}
