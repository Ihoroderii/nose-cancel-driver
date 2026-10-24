#include <CoreAudio/AudioHardware.h>
#include <CoreFoundation/CoreFoundation.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::string cfstring_to_utf8(CFStringRef value) {
    if (value == nullptr) {
        return {};
    }

    const CFIndex length = CFStringGetLength(value);
    const CFIndex max_size =
        CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
    std::vector<char> buffer(static_cast<std::size_t>(max_size), '\0');
    if (!CFStringGetCString(value, buffer.data(), max_size, kCFStringEncodingUTF8)) {
        return {};
    }
    return std::string(buffer.data());
}

template <typename T>
bool get_device_property(AudioObjectID object_id, AudioObjectPropertySelector selector,
                         AudioObjectPropertyScope scope, T& value) {
    AudioObjectPropertyAddress address{
        selector,
        scope,
        kAudioObjectPropertyElementMain,
    };

    UInt32 size = sizeof(T);
    return AudioObjectGetPropertyData(object_id, &address, 0, nullptr, &size, &value) == noErr;
}

std::string get_cfstring_property(AudioObjectID object_id, AudioObjectPropertySelector selector,
                                  AudioObjectPropertyScope scope) {
    AudioObjectPropertyAddress address{
        selector,
        scope,
        kAudioObjectPropertyElementMain,
    };

    CFStringRef value = nullptr;
    UInt32 size = sizeof(value);
    if (AudioObjectGetPropertyData(object_id, &address, 0, nullptr, &size, &value) != noErr ||
        value == nullptr) {
        return {};
    }

    std::string utf8 = cfstring_to_utf8(value);
    CFRelease(value);
    return utf8;
}

bool device_has_scope(AudioObjectID device_id, AudioObjectPropertyScope scope) {
    AudioObjectPropertyAddress address{
        kAudioDevicePropertyStreams,
        scope,
        kAudioObjectPropertyElementMain,
    };
    return AudioObjectHasProperty(device_id, &address);
}

std::size_t stream_count(AudioObjectID device_id, AudioObjectPropertyScope scope) {
    AudioObjectPropertyAddress address{
        kAudioDevicePropertyStreams,
        scope,
        kAudioObjectPropertyElementMain,
    };

    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(device_id, &address, 0, nullptr, &size) != noErr || size == 0) {
        return 0;
    }
    return static_cast<std::size_t>(size / sizeof(AudioObjectID));
}

}  // namespace

int main() {
    AudioObjectPropertyAddress address{
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain,
    };

    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, nullptr, &size) != noErr ||
        size == 0) {
        std::cerr << "Failed to query audio device list size.\n";
        return 1;
    }

    std::vector<AudioObjectID> device_ids(size / sizeof(AudioObjectID), kAudioObjectUnknown);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size,
                                   device_ids.data()) != noErr) {
        std::cerr << "Failed to query audio device list.\n";
        return 1;
    }

    for (AudioObjectID device_id : device_ids) {
        const std::string name =
            get_cfstring_property(device_id, kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal);
        const std::string uid =
            get_cfstring_property(device_id, kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal);

        UInt32 alive = 0;
        UInt32 hidden = 0;
        UInt32 can_default_input = 0;
        UInt32 transport = 0;
        Float64 sample_rate = 0.0;
        get_device_property(device_id, kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal,
                            alive);
        get_device_property(device_id, kAudioDevicePropertyTransportType, kAudioObjectPropertyScopeGlobal,
                            transport);
        get_device_property(device_id, kAudioDevicePropertyNominalSampleRate,
                            kAudioObjectPropertyScopeGlobal, sample_rate);
        get_device_property(device_id, kAudioDevicePropertyDeviceCanBeDefaultDevice,
                            kAudioObjectPropertyScopeInput, can_default_input);

        AudioObjectPropertyAddress hidden_address{
            kAudioDevicePropertyIsHidden,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain,
        };
        UInt32 hidden_size = sizeof(hidden);
        if (AudioObjectHasProperty(device_id, &hidden_address)) {
            (void)AudioObjectGetPropertyData(device_id, &hidden_address, 0, nullptr, &hidden_size,
                                             &hidden);
        }

        const std::size_t input_streams = stream_count(device_id, kAudioObjectPropertyScopeInput);
        const std::size_t output_streams = stream_count(device_id, kAudioObjectPropertyScopeOutput);

        std::cout << "Device " << device_id << "\n";
        std::cout << "  name: " << name << "\n";
        std::cout << "  uid: " << uid << "\n";
        std::cout << "  alive: " << alive << "\n";
        std::cout << "  hidden: " << hidden << "\n";
        std::cout << "  can default input: " << can_default_input << "\n";
        std::cout << "  sample rate: " << sample_rate << "\n";
        std::cout << "  transport: " << transport << "\n";
        std::cout << "  input streams: " << input_streams << "\n";
        std::cout << "  output streams: " << output_streams << "\n";
    }

    return 0;
}
