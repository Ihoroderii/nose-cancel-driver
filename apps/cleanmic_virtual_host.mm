#import <AVFoundation/AVFoundation.h>

#include "cleanmic/io/WavFile.h"
#include "cleanmic/platform/VirtualMicrophone.h"
#include "cleanmic/realtime/StreamingProcessor.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <csignal>
#include <cctype>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kCleanMicVirtualSampleRate = 48000;

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
    bool save_files = false;
    std::string save_prefix = "cleanmic_session";
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

    throw std::runtime_error("Unknown host mode: " + value + ". Use hush, rnnoise, or bypass.");
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
              << "  cleanmic_virtual_host [--mode hush|rnnoise|bypass]\n"
              << "  cleanmic_virtual_host [--input-gain-db 0]\n"
              << "  cleanmic_virtual_host [--save-prefix cleanmic_session]\n";
}

Options parse_options(int argc, char** argv) {
    Options options;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--mode" && i + 1 < argc) {
            options.mode = parse_mode(argv[++i]);
        } else if (arg == "--input-gain-db" && i + 1 < argc) {
            options.input_gain_db = std::stof(argv[++i]);
        } else if (arg == "--atten-lim-db" && i + 1 < argc) {
            options.hush_attenuation_limit_db = std::stof(argv[++i]);
        } else if (arg == "--save-prefix" && i + 1 < argc) {
            options.save_prefix = argv[++i];
            options.save_files = true;
        } else if (arg == "--no-save") {
            options.save_files = false;
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

            auto virtual_mic = cleanmic::platform::create_virtual_microphone();
            if (!virtual_mic->open(kCleanMicVirtualSampleRate, 1)) {
                throw std::runtime_error("Failed to open CleanMic shared feed: " +
                                         virtual_mic->diagnostic());
            }
            cleanmic::platform::VirtualMicrophone* virtual_mic_ptr = virtual_mic.get();

            AVAudioEngine* engine = [[AVAudioEngine alloc] init];
            engine.mainMixerNode.outputVolume = 0.0f;

            AVAudioInputNode* input_node = [engine inputNode];
            if (input_node == nil) {
                throw std::runtime_error("No audio input node is available.");
            }

            AVAudioFormat* hardware_input_format = [input_node inputFormatForBus:0];
            if (hardware_input_format == nil || hardware_input_format.channelCount == 0) {
                throw std::runtime_error("Failed to query the hardware input format.");
            }

            const AVAudioChannelCount input_channels = hardware_input_format.channelCount;
            auto processor = std::make_shared<cleanmic::realtime::StreamingProcessor>(
                to_backend(options.mode), kCleanMicVirtualSampleRate,
                options.hush_attenuation_limit_db);
            const std::size_t frame_size = processor->frame_size();
            const float input_gain = db_to_gain(options.input_gain_db);

            AVAudioFormat* tap_format =
                [[AVAudioFormat alloc] initStandardFormatWithSampleRate:kCleanMicVirtualSampleRate
                                                               channels:input_channels];

            auto raw_capture = std::make_shared<std::vector<float>>();
            auto processed_capture = std::make_shared<std::vector<float>>();
            auto capture_mutex = std::make_shared<std::mutex>();

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
                                           if (!processed.empty()) {
                                               virtual_mic_ptr->write(processed.data(),
                                                                     processed.size());
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

            std::cout << "CleanMic host backend: " << backend_name(options.mode) << '\n';
            std::cout << "Physical input channels: " << input_channels << '\n';
            if (input_channels > 1) {
                std::cout << "Capture mix: first input channel only\n";
            }
            std::cout << "Published stream: 48000 Hz mono\n";
            std::cout << "Processing frame size: " << frame_size << " samples\n";
            std::cout << "Input gain: " << options.input_gain_db << " dB\n";
            std::cout << "Virtual mic feed: " << virtual_mic->diagnostic() << '\n';
            std::cout << "Start Teams, select CleanMic as the microphone, and keep this process "
                         "running. Press Ctrl+C to stop.\n";

            while (!g_should_stop.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            [input_node removeTapOnBus:0];
            [engine stop];
            virtual_mic->close();

            if (options.save_files) {
                std::lock_guard<std::mutex> lock(*capture_mutex);

                cleanmic::io::WavData raw_wav;
                raw_wav.sample_rate = kCleanMicVirtualSampleRate;
                raw_wav.channels = 1;
                raw_wav.samples = *raw_capture;
                cleanmic::io::write_wav_file(add_suffix_to_path(options.save_prefix, "_raw"), raw_wav);

                cleanmic::io::WavData processed_wav;
                processed_wav.sample_rate = kCleanMicVirtualSampleRate;
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
