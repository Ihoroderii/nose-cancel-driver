#include "cleanmic/ipc/SharedAudioPipe.h"

#include <CoreAudio/AudioHardware.h>
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreAudio/HostTime.h>
#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <vector>

namespace {

constexpr AudioObjectID kCleanMicDeviceObjectID = 2;
constexpr AudioObjectID kCleanMicStreamObjectID = 3;
constexpr Float64 kCleanMicNominalSampleRate = 48000.0;
constexpr UInt32 kCleanMicChannels = 1;
constexpr UInt32 kCleanMicBufferFrameSize = 480;
constexpr UInt32 kCleanMicZeroTimeStampPeriod = 11520;

CFStringRef cleanmic_plugin_name() {
    return CFSTR("CleanMic");
}

CFStringRef cleanmic_plugin_manufacturer() {
    return CFSTR("Ihor Oderii");
}

CFStringRef cleanmic_plugin_bundle_id() {
    return CFSTR("com.ihoroderii.cleanmic.driver");
}

CFStringRef cleanmic_device_name() {
    return CFSTR("CleanMic");
}

CFStringRef cleanmic_device_uid() {
    return CFSTR("com.ihoroderii.cleanmic.device");
}

CFStringRef cleanmic_model_uid() {
    return CFSTR("com.ihoroderii.cleanmic.model");
}

CFStringRef cleanmic_stream_name() {
    return CFSTR("CleanMic Input");
}

bool uuid_equal(REFIID lhs, CFUUIDRef rhs) {
    const CFUUIDBytes rhs_bytes = CFUUIDGetUUIDBytes(rhs);
    return std::memcmp(&lhs, &rhs_bytes, sizeof(CFUUIDBytes)) == 0;
}

CFStringRef retained(CFStringRef value) {
    CFRetain(value);
    return value;
}

bool qualifier_allows_class(const void* qualifier_data, UInt32 qualifier_data_size, AudioClassID class_id) {
    if (qualifier_data == nullptr || qualifier_data_size == 0) {
        return true;
    }

    const auto* classes = static_cast<const AudioClassID*>(qualifier_data);
    const UInt32 count = qualifier_data_size / sizeof(AudioClassID);
    for (UInt32 index = 0; index < count; ++index) {
        if (classes[index] == class_id || classes[index] == kAudioObjectClassID) {
            return true;
        }
    }
    return false;
}

template <typename T>
OSStatus store_scalar(const T& value, UInt32 in_data_size, UInt32* out_data_size, void* out_data) {
    if (in_data_size < sizeof(T) || out_data == nullptr || out_data_size == nullptr) {
        return kAudioHardwareBadPropertySizeError;
    }
    *static_cast<T*>(out_data) = value;
    *out_data_size = sizeof(T);
    return noErr;
}

template <typename T>
OSStatus store_array(const T* values, UInt32 value_count, UInt32 in_data_size, UInt32* out_data_size,
                     void* out_data) {
    const UInt32 required_size = value_count * static_cast<UInt32>(sizeof(T));
    if (in_data_size < required_size || out_data == nullptr || out_data_size == nullptr) {
        return kAudioHardwareBadPropertySizeError;
    }
    if (required_size > 0) {
        std::memcpy(out_data, values, required_size);
    }
    *out_data_size = required_size;
    return noErr;
}

AudioStreamBasicDescription cleanmic_stream_format() {
    AudioStreamBasicDescription description{};
    description.mSampleRate = kCleanMicNominalSampleRate;
    description.mFormatID = kAudioFormatLinearPCM;
    description.mFormatFlags =
        kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved;
    description.mBytesPerPacket = sizeof(Float32);
    description.mFramesPerPacket = 1;
    description.mBytesPerFrame = sizeof(Float32);
    description.mChannelsPerFrame = kCleanMicChannels;
    description.mBitsPerChannel = 8 * sizeof(Float32);
    return description;
}

AudioStreamRangedDescription cleanmic_stream_ranged_format() {
    AudioStreamRangedDescription description{};
    description.mFormat = cleanmic_stream_format();
    description.mSampleRateRange.mMinimum = kCleanMicNominalSampleRate;
    description.mSampleRateRange.mMaximum = kCleanMicNominalSampleRate;
    return description;
}

AudioValueRange cleanmic_sample_rate_range() {
    AudioValueRange range{};
    range.mMinimum = kCleanMicNominalSampleRate;
    range.mMaximum = kCleanMicNominalSampleRate;
    return range;
}

AudioValueRange cleanmic_buffer_frame_size_range() {
    AudioValueRange range{};
    range.mMinimum = kCleanMicBufferFrameSize;
    range.mMaximum = kCleanMicBufferFrameSize;
    return range;
}

struct CleanMicDriverState {
    AudioServerPlugInDriverInterface interface{};
    std::atomic<ULONG> ref_count{1};
    AudioServerPlugInHostRef host = nullptr;
    std::mutex mutex;
    cleanmic::ipc::SharedAudioPipeReader pipe_reader{
        cleanmic::ipc::kDefaultSharedAudioPipeName};
    UInt32 active_clients = 0;
    bool io_running = false;
    UInt64 zero_timestamp_seed = 1;
    UInt64 io_start_host_time = 0;
};

CleanMicDriverState g_driver;

void notify_properties_changed(AudioObjectID object_id,
                               const std::initializer_list<AudioObjectPropertyAddress>& addresses) {
    std::lock_guard<std::mutex> lock(g_driver.mutex);
    if (g_driver.host == nullptr || addresses.size() == 0) {
        return;
    }

    std::vector<AudioObjectPropertyAddress> owned(addresses);
    g_driver.host->PropertiesChanged(g_driver.host, object_id,
                                     static_cast<UInt32>(owned.size()), owned.data());
}

bool is_device(AudioObjectID object_id) {
    return object_id == kCleanMicDeviceObjectID;
}

bool is_stream(AudioObjectID object_id) {
    return object_id == kCleanMicStreamObjectID;
}

HRESULT CleanMic_QueryInterface(void* in_driver, REFIID in_uuid, LPVOID* out_interface) {
    if (out_interface == nullptr) {
        return E_POINTER;
    }

    *out_interface = nullptr;
    if (uuid_equal(in_uuid, IUnknownUUID) ||
        uuid_equal(in_uuid, kAudioServerPlugInDriverInterfaceUUID)) {
        g_driver.ref_count.fetch_add(1, std::memory_order_relaxed);
        *out_interface = in_driver;
        return S_OK;
    }
    return E_NOINTERFACE;
}

ULONG CleanMic_AddRef(void*) {
    return g_driver.ref_count.fetch_add(1, std::memory_order_relaxed) + 1;
}

ULONG CleanMic_Release(void*) {
    const ULONG previous = g_driver.ref_count.fetch_sub(1, std::memory_order_relaxed);
    return previous > 0 ? previous - 1 : 0;
}

OSStatus CleanMic_Initialize(AudioServerPlugInDriverRef, AudioServerPlugInHostRef in_host) {
    std::lock_guard<std::mutex> lock(g_driver.mutex);
    g_driver.host = in_host;
    g_driver.pipe_reader.close();
    g_driver.active_clients = 0;
    g_driver.io_running = false;
    g_driver.zero_timestamp_seed = 1;
    g_driver.io_start_host_time = AudioGetCurrentHostTime();
    return noErr;
}

OSStatus CleanMic_CreateDevice(AudioServerPlugInDriverRef, CFDictionaryRef,
                               const AudioServerPlugInClientInfo*, AudioObjectID*) {
    return kAudioHardwareUnsupportedOperationError;
}

OSStatus CleanMic_DestroyDevice(AudioServerPlugInDriverRef, AudioObjectID) {
    return kAudioHardwareUnsupportedOperationError;
}

OSStatus CleanMic_AddDeviceClient(AudioServerPlugInDriverRef, AudioObjectID in_device_object_id,
                                  const AudioServerPlugInClientInfo*) {
    return is_device(in_device_object_id) ? noErr : kAudioHardwareBadObjectError;
}

OSStatus CleanMic_RemoveDeviceClient(AudioServerPlugInDriverRef, AudioObjectID in_device_object_id,
                                     const AudioServerPlugInClientInfo*) {
    return is_device(in_device_object_id) ? noErr : kAudioHardwareBadObjectError;
}

OSStatus CleanMic_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID,
                                                   UInt64, void*) {
    return noErr;
}

OSStatus CleanMic_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64,
                                                 void*) {
    return noErr;
}

Boolean CleanMic_HasProperty(AudioServerPlugInDriverRef, AudioObjectID in_object_id, pid_t,
                             const AudioObjectPropertyAddress* in_address) {
    if (in_address == nullptr) {
        return false;
    }

    switch (in_object_id) {
        case kAudioObjectPlugInObject:
            switch (in_address->mSelector) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                case kAudioObjectPropertyOwner:
                case kAudioObjectPropertyName:
                case kAudioObjectPropertyManufacturer:
                case kAudioObjectPropertyOwnedObjects:
                case kAudioPlugInPropertyBundleID:
                case kAudioPlugInPropertyDeviceList:
                case kAudioPlugInPropertyTranslateUIDToDevice:
                    return true;
                default:
                    return false;
            }

        case kCleanMicDeviceObjectID:
            switch (in_address->mSelector) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                case kAudioObjectPropertyOwner:
                case kAudioObjectPropertyName:
                case kAudioObjectPropertyManufacturer:
                case kAudioObjectPropertyOwnedObjects:
                case kAudioObjectPropertyControlList:
                case kAudioDevicePropertyDeviceUID:
                case kAudioDevicePropertyModelUID:
                case kAudioDevicePropertyTransportType:
                case kAudioDevicePropertyClockDomain:
                case kAudioDevicePropertyDeviceIsAlive:
                case kAudioDevicePropertyDeviceIsRunning:
                case kAudioDevicePropertyDeviceCanBeDefaultDevice:
                case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
                case kAudioDevicePropertyLatency:
                case kAudioDevicePropertyStreams:
                case kAudioDevicePropertySafetyOffset:
                case kAudioDevicePropertyNominalSampleRate:
                case kAudioDevicePropertyAvailableNominalSampleRates:
                case kAudioDevicePropertyPreferredChannelsForStereo:
                case kAudioDevicePropertyBufferFrameSize:
                case kAudioDevicePropertyBufferFrameSizeRange:
                case kAudioDevicePropertyStreamConfiguration:
                case kAudioDevicePropertyZeroTimeStampPeriod:
                    return true;
                default:
                    return false;
            }

        case kCleanMicStreamObjectID:
            switch (in_address->mSelector) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                case kAudioObjectPropertyOwner:
                case kAudioObjectPropertyName:
                case kAudioStreamPropertyIsActive:
                case kAudioStreamPropertyDirection:
                case kAudioStreamPropertyTerminalType:
                case kAudioStreamPropertyStartingChannel:
                case kAudioStreamPropertyLatency:
                case kAudioStreamPropertyVirtualFormat:
                case kAudioStreamPropertyAvailableVirtualFormats:
                case kAudioStreamPropertyPhysicalFormat:
                case kAudioStreamPropertyAvailablePhysicalFormats:
                    return true;
                default:
                    return false;
            }

        default:
            return false;
    }
}

OSStatus CleanMic_IsPropertySettable(AudioServerPlugInDriverRef, AudioObjectID in_object_id, pid_t,
                                     const AudioObjectPropertyAddress* in_address,
                                     Boolean* out_is_settable) {
    if (in_address == nullptr || out_is_settable == nullptr) {
        return kAudioHardwareIllegalOperationError;
    }

    *out_is_settable = false;

    if (in_object_id == kCleanMicDeviceObjectID) {
        switch (in_address->mSelector) {
            case kAudioDevicePropertyNominalSampleRate:
            case kAudioDevicePropertyBufferFrameSize:
                *out_is_settable = true;
                return noErr;
            default:
                return noErr;
        }
    }

    if (in_object_id == kCleanMicStreamObjectID) {
        switch (in_address->mSelector) {
            case kAudioStreamPropertyVirtualFormat:
            case kAudioStreamPropertyPhysicalFormat:
                *out_is_settable = true;
                return noErr;
            default:
                return noErr;
        }
    }

    return noErr;
}

OSStatus CleanMic_GetPropertyDataSize(AudioServerPlugInDriverRef, AudioObjectID in_object_id, pid_t,
                                      const AudioObjectPropertyAddress* in_address,
                                      UInt32 in_qualifier_data_size,
                                      const void* in_qualifier_data,
                                      UInt32* out_data_size) {
    if (in_address == nullptr || out_data_size == nullptr) {
        return kAudioHardwareIllegalOperationError;
    }

    switch (in_object_id) {
        case kAudioObjectPlugInObject:
            switch (in_address->mSelector) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                case kAudioObjectPropertyOwner:
                case kAudioPlugInPropertyTranslateUIDToDevice:
                    *out_data_size = sizeof(AudioObjectID);
                    return noErr;
                case kAudioObjectPropertyName:
                case kAudioObjectPropertyManufacturer:
                case kAudioPlugInPropertyBundleID:
                    *out_data_size = sizeof(CFStringRef);
                    return noErr;
                case kAudioObjectPropertyOwnedObjects:
                case kAudioPlugInPropertyDeviceList:
                    *out_data_size =
                        qualifier_allows_class(in_qualifier_data, in_qualifier_data_size,
                                               kAudioDeviceClassID)
                            ? sizeof(AudioObjectID)
                            : 0;
                    return noErr;
                default:
                    return kAudioHardwareUnknownPropertyError;
            }

        case kCleanMicDeviceObjectID:
            switch (in_address->mSelector) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                case kAudioObjectPropertyOwner:
                case kAudioDevicePropertyTransportType:
                case kAudioDevicePropertyClockDomain:
                case kAudioDevicePropertyDeviceIsAlive:
                case kAudioDevicePropertyDeviceIsRunning:
                case kAudioDevicePropertyDeviceCanBeDefaultDevice:
                case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
                case kAudioDevicePropertyLatency:
                case kAudioDevicePropertySafetyOffset:
                case kAudioDevicePropertyPreferredChannelsForStereo:
                case kAudioDevicePropertyBufferFrameSize:
                case kAudioDevicePropertyZeroTimeStampPeriod:
                    *out_data_size = sizeof(UInt32);
                    return noErr;
                case kAudioDevicePropertyNominalSampleRate:
                    *out_data_size = sizeof(Float64);
                    return noErr;
                case kAudioObjectPropertyName:
                case kAudioObjectPropertyManufacturer:
                case kAudioDevicePropertyDeviceUID:
                case kAudioDevicePropertyModelUID:
                    *out_data_size = sizeof(CFStringRef);
                    return noErr;
                case kAudioObjectPropertyOwnedObjects:
                    *out_data_size =
                        qualifier_allows_class(in_qualifier_data, in_qualifier_data_size,
                                               kAudioStreamClassID)
                            ? sizeof(AudioObjectID)
                            : 0;
                    return noErr;
                case kAudioObjectPropertyControlList:
                    *out_data_size = 0;
                    return noErr;
                case kAudioDevicePropertyStreams:
                    if (in_address->mScope == kAudioObjectPropertyScopeOutput) {
                        *out_data_size = 0;
                    } else {
                        *out_data_size = sizeof(AudioObjectID);
                    }
                    return noErr;
                case kAudioDevicePropertyAvailableNominalSampleRates:
                case kAudioDevicePropertyBufferFrameSizeRange:
                    *out_data_size = sizeof(AudioValueRange);
                    return noErr;
                case kAudioDevicePropertyStreamConfiguration:
                    if (in_address->mScope == kAudioObjectPropertyScopeOutput) {
                        *out_data_size = offsetof(AudioBufferList, mBuffers);
                    } else {
                        *out_data_size = offsetof(AudioBufferList, mBuffers) + sizeof(AudioBuffer);
                    }
                    return noErr;
                default:
                    return kAudioHardwareUnknownPropertyError;
            }

        case kCleanMicStreamObjectID:
            switch (in_address->mSelector) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                case kAudioObjectPropertyOwner:
                case kAudioStreamPropertyIsActive:
                case kAudioStreamPropertyDirection:
                case kAudioStreamPropertyTerminalType:
                case kAudioStreamPropertyStartingChannel:
                case kAudioStreamPropertyLatency:
                    *out_data_size = sizeof(UInt32);
                    return noErr;
                case kAudioObjectPropertyName:
                    *out_data_size = sizeof(CFStringRef);
                    return noErr;
                case kAudioStreamPropertyVirtualFormat:
                case kAudioStreamPropertyPhysicalFormat:
                    *out_data_size = sizeof(AudioStreamBasicDescription);
                    return noErr;
                case kAudioStreamPropertyAvailableVirtualFormats:
                case kAudioStreamPropertyAvailablePhysicalFormats:
                    *out_data_size = sizeof(AudioStreamRangedDescription);
                    return noErr;
                default:
                    return kAudioHardwareUnknownPropertyError;
            }

        default:
            return kAudioHardwareBadObjectError;
    }
}

OSStatus CleanMic_GetPropertyData(AudioServerPlugInDriverRef, AudioObjectID in_object_id, pid_t,
                                  const AudioObjectPropertyAddress* in_address,
                                  UInt32 in_qualifier_data_size,
                                  const void* in_qualifier_data, UInt32 in_data_size,
                                  UInt32* out_data_size, void* out_data) {
    if (in_address == nullptr || out_data_size == nullptr) {
        return kAudioHardwareIllegalOperationError;
    }
    *out_data_size = 0;

    switch (in_object_id) {
        case kAudioObjectPlugInObject:
            switch (in_address->mSelector) {
                case kAudioObjectPropertyBaseClass:
                    return store_scalar<AudioClassID>(kAudioObjectClassID, in_data_size, out_data_size, out_data);
                case kAudioObjectPropertyClass:
                    return store_scalar<AudioClassID>(kAudioPlugInClassID, in_data_size, out_data_size, out_data);
                case kAudioObjectPropertyOwner:
                    return store_scalar<AudioObjectID>(kAudioObjectSystemObject, in_data_size, out_data_size,
                                                       out_data);
                case kAudioObjectPropertyName: {
                    const CFStringRef value = retained(cleanmic_plugin_name());
                    return store_scalar<CFStringRef>(value, in_data_size, out_data_size, out_data);
                }
                case kAudioObjectPropertyManufacturer: {
                    const CFStringRef value = retained(cleanmic_plugin_manufacturer());
                    return store_scalar<CFStringRef>(value, in_data_size, out_data_size, out_data);
                }
                case kAudioObjectPropertyOwnedObjects:
                case kAudioPlugInPropertyDeviceList:
                    if (!qualifier_allows_class(in_qualifier_data, in_qualifier_data_size,
                                                kAudioDeviceClassID)) {
                        *out_data_size = 0;
                        return noErr;
                    }
                    return store_scalar<AudioObjectID>(kCleanMicDeviceObjectID, in_data_size, out_data_size,
                                                       out_data);
                case kAudioPlugInPropertyBundleID: {
                    const CFStringRef value = retained(cleanmic_plugin_bundle_id());
                    return store_scalar<CFStringRef>(value, in_data_size, out_data_size, out_data);
                }
                case kAudioPlugInPropertyTranslateUIDToDevice: {
                    if (in_qualifier_data_size < sizeof(CFStringRef) || in_qualifier_data == nullptr) {
                        return kAudioHardwareIllegalOperationError;
                    }
                    const CFStringRef requested_uid = *static_cast<const CFStringRef*>(in_qualifier_data);
                    const AudioObjectID result =
                        CFEqual(requested_uid, cleanmic_device_uid()) ? kCleanMicDeviceObjectID
                                                                      : kAudioObjectUnknown;
                    return store_scalar<AudioObjectID>(result, in_data_size, out_data_size, out_data);
                }
                default:
                    return kAudioHardwareUnknownPropertyError;
            }

        case kCleanMicDeviceObjectID:
            switch (in_address->mSelector) {
                case kAudioObjectPropertyBaseClass:
                    return store_scalar<AudioClassID>(kAudioObjectClassID, in_data_size, out_data_size, out_data);
                case kAudioObjectPropertyClass:
                    return store_scalar<AudioClassID>(kAudioDeviceClassID, in_data_size, out_data_size, out_data);
                case kAudioObjectPropertyOwner:
                    return store_scalar<AudioObjectID>(kAudioObjectPlugInObject, in_data_size, out_data_size,
                                                       out_data);
                case kAudioObjectPropertyName: {
                    const CFStringRef value = retained(cleanmic_device_name());
                    return store_scalar<CFStringRef>(value, in_data_size, out_data_size, out_data);
                }
                case kAudioObjectPropertyManufacturer: {
                    const CFStringRef value = retained(cleanmic_plugin_manufacturer());
                    return store_scalar<CFStringRef>(value, in_data_size, out_data_size, out_data);
                }
                case kAudioObjectPropertyOwnedObjects:
                    if (!qualifier_allows_class(in_qualifier_data, in_qualifier_data_size,
                                                kAudioStreamClassID)) {
                        *out_data_size = 0;
                        return noErr;
                    }
                    return store_scalar<AudioObjectID>(kCleanMicStreamObjectID, in_data_size, out_data_size,
                                                       out_data);
                case kAudioObjectPropertyControlList:
                    *out_data_size = 0;
                    return noErr;
                case kAudioDevicePropertyDeviceUID: {
                    const CFStringRef value = retained(cleanmic_device_uid());
                    return store_scalar<CFStringRef>(value, in_data_size, out_data_size, out_data);
                }
                case kAudioDevicePropertyModelUID: {
                    const CFStringRef value = retained(cleanmic_model_uid());
                    return store_scalar<CFStringRef>(value, in_data_size, out_data_size, out_data);
                }
                case kAudioDevicePropertyTransportType:
                    return store_scalar<UInt32>(kAudioDeviceTransportTypeVirtual, in_data_size, out_data_size,
                                                out_data);
                case kAudioDevicePropertyClockDomain:
                    return store_scalar<UInt32>(0, in_data_size, out_data_size, out_data);
                case kAudioDevicePropertyDeviceIsAlive:
                    return store_scalar<UInt32>(1, in_data_size, out_data_size, out_data);
                case kAudioDevicePropertyDeviceIsRunning: {
                    const UInt32 running = g_driver.io_running ? 1U : 0U;
                    return store_scalar<UInt32>(running, in_data_size, out_data_size, out_data);
                }
                case kAudioDevicePropertyDeviceCanBeDefaultDevice: {
                    const UInt32 can_be_default =
                        (in_address->mScope == kAudioObjectPropertyScopeOutput) ? 0U : 1U;
                    return store_scalar<UInt32>(can_be_default, in_data_size, out_data_size, out_data);
                }
                case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
                    return store_scalar<UInt32>(0, in_data_size, out_data_size, out_data);
                case kAudioDevicePropertyLatency:
                    return store_scalar<UInt32>(0, in_data_size, out_data_size, out_data);
                case kAudioDevicePropertyStreams:
                    if (in_address->mScope == kAudioObjectPropertyScopeOutput) {
                        *out_data_size = 0;
                        return noErr;
                    }
                    return store_scalar<AudioObjectID>(kCleanMicStreamObjectID, in_data_size, out_data_size,
                                                       out_data);
                case kAudioDevicePropertySafetyOffset:
                    return store_scalar<UInt32>(0, in_data_size, out_data_size, out_data);
                case kAudioDevicePropertyNominalSampleRate:
                    return store_scalar<Float64>(kCleanMicNominalSampleRate, in_data_size, out_data_size, out_data);
                case kAudioDevicePropertyAvailableNominalSampleRates: {
                    const AudioValueRange range = cleanmic_sample_rate_range();
                    return store_scalar<AudioValueRange>(range, in_data_size, out_data_size, out_data);
                }
                case kAudioDevicePropertyPreferredChannelsForStereo: {
                    const std::array<UInt32, 2> channels = {1, 1};
                    return store_array<UInt32>(channels.data(), channels.size(), in_data_size, out_data_size,
                                               out_data);
                }
                case kAudioDevicePropertyBufferFrameSize:
                    return store_scalar<UInt32>(kCleanMicBufferFrameSize, in_data_size, out_data_size, out_data);
                case kAudioDevicePropertyBufferFrameSizeRange: {
                    const AudioValueRange range = cleanmic_buffer_frame_size_range();
                    return store_scalar<AudioValueRange>(range, in_data_size, out_data_size, out_data);
                }
                case kAudioDevicePropertyStreamConfiguration: {
                    const bool has_input_stream =
                        in_address->mScope != kAudioObjectPropertyScopeOutput;
                    const UInt32 required_size =
                        has_input_stream ? offsetof(AudioBufferList, mBuffers) + sizeof(AudioBuffer)
                                         : offsetof(AudioBufferList, mBuffers);
                    if (in_data_size < required_size || out_data == nullptr) {
                        return kAudioHardwareBadPropertySizeError;
                    }

                    auto* buffer_list = static_cast<AudioBufferList*>(out_data);
                    buffer_list->mNumberBuffers = has_input_stream ? 1 : 0;
                    if (has_input_stream) {
                        buffer_list->mBuffers[0].mNumberChannels = kCleanMicChannels;
                        buffer_list->mBuffers[0].mDataByteSize = 0;
                        buffer_list->mBuffers[0].mData = nullptr;
                    }
                    *out_data_size = required_size;
                    return noErr;
                }
                case kAudioDevicePropertyZeroTimeStampPeriod:
                    return store_scalar<UInt32>(kCleanMicZeroTimeStampPeriod, in_data_size, out_data_size, out_data);
                default:
                    return kAudioHardwareUnknownPropertyError;
            }

        case kCleanMicStreamObjectID:
            switch (in_address->mSelector) {
                case kAudioObjectPropertyBaseClass:
                    return store_scalar<AudioClassID>(kAudioObjectClassID, in_data_size, out_data_size, out_data);
                case kAudioObjectPropertyClass:
                    return store_scalar<AudioClassID>(kAudioStreamClassID, in_data_size, out_data_size, out_data);
                case kAudioObjectPropertyOwner:
                    return store_scalar<AudioObjectID>(kCleanMicDeviceObjectID, in_data_size, out_data_size,
                                                       out_data);
                case kAudioObjectPropertyName: {
                    const CFStringRef value = retained(cleanmic_stream_name());
                    return store_scalar<CFStringRef>(value, in_data_size, out_data_size, out_data);
                }
                case kAudioStreamPropertyIsActive:
                    return store_scalar<UInt32>(1, in_data_size, out_data_size, out_data);
                case kAudioStreamPropertyDirection:
                    return store_scalar<UInt32>(1, in_data_size, out_data_size, out_data);
                case kAudioStreamPropertyTerminalType:
                    return store_scalar<UInt32>(kAudioStreamTerminalTypeMicrophone, in_data_size, out_data_size,
                                                out_data);
                case kAudioStreamPropertyStartingChannel:
                    return store_scalar<UInt32>(1, in_data_size, out_data_size, out_data);
                case kAudioStreamPropertyLatency:
                    return store_scalar<UInt32>(0, in_data_size, out_data_size, out_data);
                case kAudioStreamPropertyVirtualFormat:
                case kAudioStreamPropertyPhysicalFormat: {
                    const AudioStreamBasicDescription description = cleanmic_stream_format();
                    return store_scalar<AudioStreamBasicDescription>(description, in_data_size, out_data_size,
                                                                     out_data);
                }
                case kAudioStreamPropertyAvailableVirtualFormats:
                case kAudioStreamPropertyAvailablePhysicalFormats: {
                    const AudioStreamRangedDescription description =
                        cleanmic_stream_ranged_format();
                    return store_scalar<AudioStreamRangedDescription>(description, in_data_size, out_data_size,
                                                                      out_data);
                }
                default:
                    return kAudioHardwareUnknownPropertyError;
            }

        default:
            return kAudioHardwareBadObjectError;
    }
}

OSStatus CleanMic_SetPropertyData(AudioServerPlugInDriverRef, AudioObjectID in_object_id, pid_t,
                                  const AudioObjectPropertyAddress* in_address,
                                  UInt32, const void*, UInt32 in_data_size,
                                  const void* in_data) {
    if (in_address == nullptr || in_data == nullptr) {
        return kAudioHardwareIllegalOperationError;
    }

    if (in_object_id == kCleanMicDeviceObjectID) {
        switch (in_address->mSelector) {
            case kAudioDevicePropertyNominalSampleRate: {
                if (in_data_size < sizeof(Float64)) {
                    return kAudioHardwareBadPropertySizeError;
                }
                const Float64 requested = *static_cast<const Float64*>(in_data);
                return std::fabs(requested - kCleanMicNominalSampleRate) < 0.5 ? noErr
                                                                               : kAudioHardwareIllegalOperationError;
            }
            case kAudioDevicePropertyBufferFrameSize: {
                if (in_data_size < sizeof(UInt32)) {
                    return kAudioHardwareBadPropertySizeError;
                }
                const UInt32 requested = *static_cast<const UInt32*>(in_data);
                return requested == kCleanMicBufferFrameSize ? noErr
                                                             : kAudioHardwareIllegalOperationError;
            }
            default:
                return kAudioHardwareIllegalOperationError;
        }
    }

    if (in_object_id == kCleanMicStreamObjectID) {
        switch (in_address->mSelector) {
            case kAudioStreamPropertyVirtualFormat:
            case kAudioStreamPropertyPhysicalFormat: {
                if (in_data_size < sizeof(AudioStreamBasicDescription)) {
                    return kAudioHardwareBadPropertySizeError;
                }
                const auto* requested = static_cast<const AudioStreamBasicDescription*>(in_data);
                const AudioStreamBasicDescription current = cleanmic_stream_format();
                return std::memcmp(requested, &current, sizeof(current)) == 0
                           ? noErr
                           : kAudioHardwareIllegalOperationError;
            }
            default:
                return kAudioHardwareIllegalOperationError;
        }
    }

    return kAudioHardwareIllegalOperationError;
}

OSStatus CleanMic_StartIO(AudioServerPlugInDriverRef, AudioObjectID in_device_object_id,
                          UInt32) {
    if (!is_device(in_device_object_id)) {
        return kAudioHardwareBadObjectError;
    }

    bool notify = false;
    {
        std::lock_guard<std::mutex> lock(g_driver.mutex);
        if (g_driver.active_clients == 0) {
            g_driver.io_running = true;
            g_driver.zero_timestamp_seed += 1;
            g_driver.io_start_host_time = AudioGetCurrentHostTime();
            notify = true;
        }
        g_driver.active_clients += 1;
        if (!g_driver.pipe_reader.is_open()) {
            g_driver.pipe_reader.open();
        }
    }

    if (notify) {
        notify_properties_changed(
            kCleanMicDeviceObjectID,
            {AudioObjectPropertyAddress{kAudioDevicePropertyDeviceIsRunning,
                                        kAudioObjectPropertyScopeGlobal,
                                        kAudioObjectPropertyElementMain}});
    }
    return noErr;
}

OSStatus CleanMic_StopIO(AudioServerPlugInDriverRef, AudioObjectID in_device_object_id, UInt32) {
    if (!is_device(in_device_object_id)) {
        return kAudioHardwareBadObjectError;
    }

    bool notify = false;
    {
        std::lock_guard<std::mutex> lock(g_driver.mutex);
        if (g_driver.active_clients > 0) {
            g_driver.active_clients -= 1;
        }
        if (g_driver.active_clients == 0 && g_driver.io_running) {
            g_driver.io_running = false;
            notify = true;
        }
    }

    if (notify) {
        notify_properties_changed(
            kCleanMicDeviceObjectID,
            {AudioObjectPropertyAddress{kAudioDevicePropertyDeviceIsRunning,
                                        kAudioObjectPropertyScopeGlobal,
                                        kAudioObjectPropertyElementMain}});
    }
    return noErr;
}

OSStatus CleanMic_GetZeroTimeStamp(AudioServerPlugInDriverRef, AudioObjectID in_device_object_id,
                                   UInt32, Float64* out_sample_time, UInt64* out_host_time,
                                   UInt64* out_seed) {
    if (!is_device(in_device_object_id) || out_sample_time == nullptr || out_host_time == nullptr ||
        out_seed == nullptr) {
        return kAudioHardwareIllegalOperationError;
    }

    UInt64 start_host_time = 0;
    UInt64 seed = 0;
    {
        std::lock_guard<std::mutex> lock(g_driver.mutex);
        start_host_time = g_driver.io_start_host_time;
        seed = g_driver.zero_timestamp_seed;
    }

    const UInt64 now = AudioGetCurrentHostTime();
    const UInt64 elapsed_host_time = now > start_host_time ? (now - start_host_time) : 0;
    const UInt64 elapsed_nanos = AudioConvertHostTimeToNanos(elapsed_host_time);
    const Float64 elapsed_frames =
        (static_cast<Float64>(elapsed_nanos) * kCleanMicNominalSampleRate) / 1.0e9;
    const Float64 aligned_frames =
        std::floor(elapsed_frames / kCleanMicZeroTimeStampPeriod) * kCleanMicZeroTimeStampPeriod;
    const UInt64 aligned_nanos = static_cast<UInt64>((aligned_frames / kCleanMicNominalSampleRate) *
                                                     1.0e9);

    *out_sample_time = aligned_frames;
    *out_host_time = start_host_time + AudioConvertNanosToHostTime(aligned_nanos);
    *out_seed = seed;
    return noErr;
}

OSStatus CleanMic_WillDoIOOperation(AudioServerPlugInDriverRef, AudioObjectID in_device_object_id,
                                    UInt32, UInt32 in_operation_id, Boolean* out_will_do,
                                    Boolean* out_will_do_in_place) {
    if (!is_device(in_device_object_id) || out_will_do == nullptr || out_will_do_in_place == nullptr) {
        return kAudioHardwareIllegalOperationError;
    }

    *out_will_do = (in_operation_id == kAudioServerPlugInIOOperationReadInput);
    *out_will_do_in_place = (in_operation_id == kAudioServerPlugInIOOperationReadInput);
    return noErr;
}

OSStatus CleanMic_BeginIOOperation(AudioServerPlugInDriverRef, AudioObjectID in_device_object_id,
                                   UInt32, UInt32, UInt32,
                                   const AudioServerPlugInIOCycleInfo*) {
    return is_device(in_device_object_id) ? noErr : kAudioHardwareBadObjectError;
}

OSStatus CleanMic_DoIOOperation(AudioServerPlugInDriverRef, AudioObjectID in_device_object_id,
                                AudioObjectID in_stream_object_id, UInt32, UInt32 in_operation_id,
                                UInt32 in_io_buffer_frame_size,
                                const AudioServerPlugInIOCycleInfo*, void* io_main_buffer,
                                void*) {
    if (!is_device(in_device_object_id) || !is_stream(in_stream_object_id)) {
        return kAudioHardwareBadObjectError;
    }

    if (in_operation_id != kAudioServerPlugInIOOperationReadInput) {
        return noErr;
    }

    if (io_main_buffer == nullptr) {
        return kAudioHardwareIllegalOperationError;
    }

    auto* buffer_list = static_cast<AudioBufferList*>(io_main_buffer);
    if (buffer_list->mNumberBuffers == 0) {
        return noErr;
    }

    if (!g_driver.pipe_reader.is_open()) {
        g_driver.pipe_reader.open();
    }

    AudioBuffer& buffer = buffer_list->mBuffers[0];
    if (buffer.mData == nullptr) {
        return noErr;
    }

    auto* samples = static_cast<float*>(buffer.mData);
    const std::size_t frames_requested = in_io_buffer_frame_size;
    g_driver.pipe_reader.read_interleaved(samples, frames_requested, true);
    buffer.mDataByteSize =
        static_cast<UInt32>(frames_requested * kCleanMicChannels * sizeof(Float32));

    for (UInt32 index = 1; index < buffer_list->mNumberBuffers; ++index) {
        if (buffer_list->mBuffers[index].mData != nullptr) {
            std::memset(buffer_list->mBuffers[index].mData, 0,
                        buffer_list->mBuffers[index].mDataByteSize);
        }
    }

    return noErr;
}

OSStatus CleanMic_EndIOOperation(AudioServerPlugInDriverRef, AudioObjectID in_device_object_id,
                                 UInt32, UInt32, UInt32,
                                 const AudioServerPlugInIOCycleInfo*) {
    return is_device(in_device_object_id) ? noErr : kAudioHardwareBadObjectError;
}

AudioServerPlugInDriverInterface g_driver_interface = {
    nullptr,
    CleanMic_QueryInterface,
    CleanMic_AddRef,
    CleanMic_Release,
    CleanMic_Initialize,
    CleanMic_CreateDevice,
    CleanMic_DestroyDevice,
    CleanMic_AddDeviceClient,
    CleanMic_RemoveDeviceClient,
    CleanMic_PerformDeviceConfigurationChange,
    CleanMic_AbortDeviceConfigurationChange,
    CleanMic_HasProperty,
    CleanMic_IsPropertySettable,
    CleanMic_GetPropertyDataSize,
    CleanMic_GetPropertyData,
    CleanMic_SetPropertyData,
    CleanMic_StartIO,
    CleanMic_StopIO,
    CleanMic_GetZeroTimeStamp,
    CleanMic_WillDoIOOperation,
    CleanMic_BeginIOOperation,
    CleanMic_DoIOOperation,
    CleanMic_EndIOOperation,
};

}  // namespace

extern "C" void* CleanMic_CreateFactory(CFAllocatorRef, CFUUIDRef requested_type_uuid) {
    if (requested_type_uuid == nullptr || !CFEqual(requested_type_uuid, kAudioServerPlugInTypeUUID)) {
        return nullptr;
    }

    g_driver.interface = g_driver_interface;
    return &g_driver.interface;
}
