cmake_minimum_required(VERSION 3.20)

project(noise_cancel_driver VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(APPLE)
    enable_language(OBJCXX)
endif()

option(CLEANMIC_ENABLE_RNNOISE "Build the RNNoise backend when vendored sources are present." ON)
option(CLEANMIC_ENABLE_HUSH "Enable the Hush backend when vendored deployment files are present." ON)

set(CLEANMIC_HAS_RNNOISE OFF)
set(CLEANMIC_HAS_HUSH OFF)

if(CLEANMIC_ENABLE_RNNOISE
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/rnnoise/include/rnnoise.h"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/rnnoise/src/rnnoise_data.c"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/rnnoise/src/rnnoise_data.h")
    add_library(rnnoise_vendor STATIC
        third_party/rnnoise/src/denoise.c
        third_party/rnnoise/src/rnn.c
        third_party/rnnoise/src/pitch.c
        third_party/rnnoise/src/kiss_fft.c
        third_party/rnnoise/src/celt_lpc.c
        third_party/rnnoise/src/nnet.c
        third_party/rnnoise/src/nnet_default.c
        third_party/rnnoise/src/parse_lpcnet_weights.c
        third_party/rnnoise/src/rnnoise_data.c
        third_party/rnnoise/src/rnnoise_tables.c
    )

    target_include_directories(rnnoise_vendor
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/rnnoise/include
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/rnnoise/src
    )

    if(NOT MSVC)
        target_link_libraries(rnnoise_vendor PUBLIC m)
    endif()

    set(CLEANMIC_HAS_RNNOISE ON)
endif()

set(CLEANMIC_HUSH_LIB_PATH "")
if(APPLE)
    set(CLEANMIC_HUSH_LIB_PATH "${CMAKE_CURRENT_SOURCE_DIR}/third_party/Hush-src/deployment/lib/libweya_nc.dylib")
elseif(WIN32)
    set(CLEANMIC_HUSH_LIB_PATH "${CMAKE_CURRENT_SOURCE_DIR}/third_party/Hush-src/deployment/lib/weya_nc.dll")
elseif(UNIX)
    set(CLEANMIC_HUSH_LIB_PATH "${CMAKE_CURRENT_SOURCE_DIR}/third_party/Hush-src/deployment/lib/libweya_nc.so")
endif()

set(CLEANMIC_HUSH_MODEL_PATH
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/Hush-src/deployment/models/advanced_dfnet16k_model_best_onnx.tar.gz")

if(CLEANMIC_ENABLE_HUSH
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/Hush-src/deployment/include/weya_nc.h"
   AND EXISTS "${CLEANMIC_HUSH_LIB_PATH}"
   AND EXISTS "${CLEANMIC_HUSH_MODEL_PATH}")
    set(CLEANMIC_HAS_HUSH ON)
endif()

add_library(cleanmic_core
    src/ipc/SharedAudioPipe.cpp
    src/io/WavFile.cpp
    src/processing/FFT.cpp
    src/processing/HushEnhancer.cpp
    src/processing/RnnoiseEnhancer.cpp
    src/processing/VoiceActivityDetector.cpp
    src/processing/VoiceEnhancer.cpp
    src/platform/VirtualMicrophoneStub.cpp
)

target_include_directories(cleanmic_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(CLEANMIC_HAS_HUSH)
    target_include_directories(cleanmic_core PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/Hush-src/deployment/include
    )
endif()

target_compile_definitions(cleanmic_core
    PUBLIC
        CLEANMIC_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}"
)

if(MSVC)
    target_compile_options(cleanmic_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(cleanmic_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(CLEANMIC_HAS_RNNOISE)
    target_compile_definitions(cleanmic_core PUBLIC CLEANMIC_HAS_RNNOISE=1)
    target_link_libraries(cleanmic_core PRIVATE rnnoise_vendor)
else()
    target_compile_definitions(cleanmic_core PUBLIC CLEANMIC_HAS_RNNOISE=0)
endif()

if(CLEANMIC_HAS_HUSH)
    target_compile_definitions(cleanmic_core
        PUBLIC
            CLEANMIC_HAS_HUSH=1
            CLEANMIC_HUSH_DEFAULT_LIB="${CLEANMIC_HUSH_LIB_PATH}"
            CLEANMIC_HUSH_DEFAULT_MODEL="${CLEANMIC_HUSH_MODEL_PATH}"
    )
else()
    target_compile_definitions(cleanmic_core PUBLIC CLEANMIC_HAS_HUSH=0)
endif()

if(UNIX AND NOT APPLE)
    target_link_libraries(cleanmic_core PRIVATE dl)
endif()

add_executable(cleanmic_cli apps/cleanmic_cli.cpp)
target_link_libraries(cleanmic_cli PRIVATE cleanmic_core)

if(APPLE)
    find_library(AVFOUNDATION_FRAMEWORK AVFoundation REQUIRED)
    find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
    find_library(COREAUDIO_FRAMEWORK CoreAudio REQUIRED)
    find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)

    add_executable(list_audio_devices apps/list_audio_devices.cpp)
    target_link_libraries(list_audio_devices
        PRIVATE
            ${COREAUDIO_FRAMEWORK}
            ${COREFOUNDATION_FRAMEWORK}
    )

    add_executable(cleanmic_realtime apps/cleanmic_realtime.mm)
    set_source_files_properties(apps/cleanmic_realtime.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    target_link_libraries(cleanmic_realtime
        PRIVATE
            cleanmic_core
            ${AVFOUNDATION_FRAMEWORK}
            ${FOUNDATION_FRAMEWORK}
    )

    add_executable(cleanmic_virtual_host apps/cleanmic_virtual_host.mm)
    set_source_files_properties(apps/cleanmic_virtual_host.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    target_link_libraries(cleanmic_virtual_host
        PRIVATE
            cleanmic_core
            ${AVFOUNDATION_FRAMEWORK}
            ${FOUNDATION_FRAMEWORK}
    )

    add_library(cleanmic_hal_plugin MODULE
        src/ipc/SharedAudioPipe.cpp
        src/platform/macos/CleanMicAudioServerPlugIn.cpp
    )
    set_target_properties(cleanmic_hal_plugin PROPERTIES
        PREFIX ""
        OUTPUT_NAME "CleanMic"
        SUFFIX ""
    )
    target_include_directories(cleanmic_hal_plugin
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(cleanmic_hal_plugin
        PRIVATE
            ${COREAUDIO_FRAMEWORK}
            ${COREFOUNDATION_FRAMEWORK}
    )

    set(CLEANMIC_DRIVER_BUNDLE_DIR "${CMAKE_CURRENT_BINARY_DIR}/CleanMic.driver")
    configure_file(macos/CleanMic.driver/Info.plist.in
        ${CMAKE_CURRENT_BINARY_DIR}/CleanMic-Info.plist
        COPYONLY
    )

    add_custom_command(TARGET cleanmic_hal_plugin POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${CLEANMIC_DRIVER_BUNDLE_DIR}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CLEANMIC_DRIVER_BUNDLE_DIR}/Contents/MacOS
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:cleanmic_hal_plugin>
                ${CLEANMIC_DRIVER_BUNDLE_DIR}/Contents/MacOS/CleanMic
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/CleanMic-Info.plist
                ${CLEANMIC_DRIVER_BUNDLE_DIR}/Contents/Info.plist
        VERBATIM
    )

    add_custom_target(cleanmic_driver_bundle ALL DEPENDS cleanmic_hal_plugin)
endif()
