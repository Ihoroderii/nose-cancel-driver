#include "cleanmic/processing/HushEnhancer.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(CLEANMIC_HAS_HUSH) && CLEANMIC_HAS_HUSH
#include "weya_nc.h"
#endif

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace cleanmic::processing {
namespace {

float db_to_gain(float db) {
    return std::pow(10.0f, db / 20.0f);
}

float compute_gated_rms(const float* samples, std::size_t count, float gate) {
    double gated_energy = 0.0;
    std::size_t gated_count = 0;
    double total_energy = 0.0;

    for (std::size_t i = 0; i < count; ++i) {
        const float sample = samples[i];
        const double energy = static_cast<double>(sample) * static_cast<double>(sample);
        total_energy += energy;
        if (std::fabs(sample) >= gate) {
            gated_energy += energy;
            ++gated_count;
        }
    }

    if (gated_count > 0) {
        return static_cast<float>(std::sqrt(gated_energy / static_cast<double>(gated_count)));
    }

    if (count == 0) {
        return 0.0f;
    }

    return static_cast<float>(std::sqrt(total_energy / static_cast<double>(count)));
}

void apply_gain(float* samples, std::size_t count, float gain) {
    for (std::size_t i = 0; i < count; ++i) {
        samples[i] *= gain;
    }
}

void apply_compressor(float* samples, std::size_t count, float threshold, float ratio) {
    if (ratio <= 1.0f || threshold <= 0.0f) {
        return;
    }

    for (std::size_t i = 0; i < count; ++i) {
        float& sample = samples[i];
        const float sign = sample < 0.0f ? -1.0f : 1.0f;
        float magnitude = std::fabs(sample);
        if (magnitude > threshold) {
            magnitude = threshold + (magnitude - threshold) / ratio;
        }
        sample = sign * magnitude;
    }
}

void limit_peak(float* samples, std::size_t count, float ceiling) {
    float peak = 0.0f;
    for (std::size_t i = 0; i < count; ++i) {
        peak = std::max(peak, std::fabs(samples[i]));
    }

    if (peak > ceiling && ceiling > 0.0f) {
        const float scale = ceiling / peak;
        for (std::size_t i = 0; i < count; ++i) {
            samples[i] *= scale;
        }
    }
}

void limit_peak(std::vector<float>& samples, float ceiling) {
    limit_peak(samples.data(), samples.size(), ceiling);
}

void apply_hush_makeup_frame(const HushEnhancerConfig& config, float* samples,
                             std::size_t count, float& current_gain) {
    const float rms = compute_gated_rms(samples, count, config.loudness_gate);
    if (rms > 1.0e-5f) {
        const float desired_gain = std::max(1.0f, config.target_rms / rms);
        const float max_gain = db_to_gain(config.max_makeup_gain_db);
        const float clamped_gain = std::min(desired_gain, max_gain);
        const float smoothing = (clamped_gain > current_gain) ? 0.20f : 0.08f;
        current_gain += (clamped_gain - current_gain) * smoothing;
    }

    apply_gain(samples, count, current_gain);
    apply_compressor(samples, count, config.compressor_threshold, config.compressor_ratio);
    limit_peak(samples, count, config.limiter_ceiling);
}

std::string getenv_or_empty(const char* key) {
    const char* value = std::getenv(key);
    return value == nullptr ? std::string{} : std::string(value);
}

std::string resolve_library_path(const HushEnhancerConfig& config) {
    if (!config.library_path.empty()) {
        return config.library_path;
    }

    const std::string env = getenv_or_empty("CLEANMIC_HUSH_LIB");
    if (!env.empty()) {
        return env;
    }

#if defined(CLEANMIC_HAS_HUSH) && CLEANMIC_HAS_HUSH
    return CLEANMIC_HUSH_DEFAULT_LIB;
#else
    return {};
#endif
}

std::string resolve_model_path(const HushEnhancerConfig& config) {
    if (!config.model_path.empty()) {
        return config.model_path;
    }

    const std::string env = getenv_or_empty("CLEANMIC_HUSH_MODEL");
    if (!env.empty()) {
        return env;
    }

#if defined(CLEANMIC_HAS_HUSH) && CLEANMIC_HAS_HUSH
    return CLEANMIC_HUSH_DEFAULT_MODEL;
#else
    return {};
#endif
}

}  // namespace

struct HushEnhancer::Impl {
#if defined(CLEANMIC_HAS_HUSH) && CLEANMIC_HAS_HUSH
    using model_load_from_path_fn = WeyaModel* (*)(const char*);
    using model_free_fn = void (*)(WeyaModel*);
    using session_create_fn = WeyaSession* (*)(const WeyaModel*, std::size_t, float);
    using session_free_fn = void (*)(WeyaSession*);
    using get_frame_length_fn = std::size_t (*)(const WeyaSession*);
    using get_sample_rate_fn = std::size_t (*)(const WeyaSession*);
    using process_frame_fn = float (*)(WeyaSession*, const float*, float*);
    using reset_fn = void (*)(WeyaSession*);

#if defined(_WIN32)
    HMODULE library_handle = nullptr;
#else
    void* library_handle = nullptr;
#endif

    model_load_from_path_fn model_load_from_path = nullptr;
    model_free_fn model_free = nullptr;
    session_create_fn session_create = nullptr;
    session_free_fn session_free = nullptr;
    get_frame_length_fn get_frame_length = nullptr;
    get_sample_rate_fn get_sample_rate = nullptr;
    process_frame_fn process_frame = nullptr;
    reset_fn reset = nullptr;

    WeyaModel* model = nullptr;
    WeyaSession* session = nullptr;
    std::size_t frame_size = 0;
    int internal_sample_rate = 16000;
    float current_makeup_gain = 1.0f;
    std::string library_path;
    std::string model_path;
#endif
};

HushEnhancer::HushEnhancer(HushEnhancerConfig config)
    : config_(std::move(config)), impl_(std::make_unique<Impl>()) {
    reset();
}

HushEnhancer::~HushEnhancer() {
#if defined(CLEANMIC_HAS_HUSH) && CLEANMIC_HAS_HUSH
    if (impl_) {
        if (impl_->session != nullptr && impl_->session_free != nullptr) {
            impl_->session_free(impl_->session);
            impl_->session = nullptr;
        }
        if (impl_->model != nullptr && impl_->model_free != nullptr) {
            impl_->model_free(impl_->model);
            impl_->model = nullptr;
        }
#if defined(_WIN32)
        if (impl_->library_handle != nullptr) {
            FreeLibrary(impl_->library_handle);
            impl_->library_handle = nullptr;
        }
#else
        if (impl_->library_handle != nullptr) {
            dlclose(impl_->library_handle);
            impl_->library_handle = nullptr;
        }
#endif
    }
#endif
}

HushEnhancer::HushEnhancer(HushEnhancer&&) noexcept = default;
HushEnhancer& HushEnhancer::operator=(HushEnhancer&&) noexcept = default;

std::vector<float> HushEnhancer::process(const std::vector<float>& input) {
#if !defined(CLEANMIC_HAS_HUSH) || !CLEANMIC_HAS_HUSH
    (void)input;
    throw std::runtime_error("Hush backend is not available in this build.");
#else
    if (input.empty()) {
        return {};
    }

    reset();

    std::vector<float> output(input.size(), 0.0f);
    std::vector<float> in_frame(impl_->frame_size, 0.0f);
    std::vector<float> out_frame(impl_->frame_size, 0.0f);

    for (std::size_t offset = 0; offset < input.size(); offset += impl_->frame_size) {
        const std::size_t count = std::min(impl_->frame_size, input.size() - offset);
        std::fill(in_frame.begin(), in_frame.end(), 0.0f);

        for (std::size_t i = 0; i < count; ++i) {
            in_frame[i] = std::clamp(input[offset + i], -1.0f, 1.0f);
        }

        process_frame(in_frame.data(), out_frame.data(), impl_->frame_size);
        std::copy_n(out_frame.begin(), count, output.begin() + static_cast<std::ptrdiff_t>(offset));
    }

    limit_peak(output, config_.limiter_ceiling);
    return output;
#endif
}

void HushEnhancer::process_frame(const float* input, float* output, std::size_t frame_count) {
#if !defined(CLEANMIC_HAS_HUSH) || !CLEANMIC_HAS_HUSH
    (void)input;
    (void)output;
    (void)frame_count;
    throw std::runtime_error("Hush backend is not available in this build.");
#else
    if (frame_count != impl_->frame_size) {
        throw std::invalid_argument("Hush frame size mismatch.");
    }

    (void)impl_->process_frame(impl_->session, input, output);
    apply_hush_makeup_frame(config_, output, frame_count, impl_->current_makeup_gain);
#endif
}

void HushEnhancer::reset() {
#if !defined(CLEANMIC_HAS_HUSH) || !CLEANMIC_HAS_HUSH
    throw std::runtime_error("Hush backend is not available in this build.");
#else
    const std::string library_path = resolve_library_path(config_);
    const std::string model_path = resolve_model_path(config_);

    if (library_path.empty() || model_path.empty()) {
        throw std::runtime_error("Hush backend paths are not configured.");
    }

    if (impl_->library_handle == nullptr) {
#if defined(_WIN32)
        impl_->library_handle = LoadLibraryA(library_path.c_str());
        if (impl_->library_handle == nullptr) {
            throw std::runtime_error("Failed to load Hush library: " + library_path);
        }

        auto load_symbol = [&](const char* name) -> FARPROC {
            FARPROC symbol = GetProcAddress(impl_->library_handle, name);
            if (symbol == nullptr) {
                throw std::runtime_error("Missing Hush symbol: " + std::string(name));
            }
            return symbol;
        };

        impl_->model_load_from_path =
            reinterpret_cast<Impl::model_load_from_path_fn>(load_symbol("weya_nc_model_load_from_path"));
        impl_->model_free =
            reinterpret_cast<Impl::model_free_fn>(load_symbol("weya_nc_model_free"));
        impl_->session_create =
            reinterpret_cast<Impl::session_create_fn>(load_symbol("weya_nc_session_create"));
        impl_->session_free =
            reinterpret_cast<Impl::session_free_fn>(load_symbol("weya_nc_session_free"));
        impl_->get_frame_length =
            reinterpret_cast<Impl::get_frame_length_fn>(load_symbol("weya_nc_get_frame_length"));
        impl_->get_sample_rate =
            reinterpret_cast<Impl::get_sample_rate_fn>(load_symbol("weya_nc_get_sample_rate"));
        impl_->process_frame =
            reinterpret_cast<Impl::process_frame_fn>(load_symbol("weya_nc_process_frame"));
        impl_->reset =
            reinterpret_cast<Impl::reset_fn>(load_symbol("weya_nc_reset"));
#else
        impl_->library_handle = dlopen(library_path.c_str(), RTLD_NOW);
        if (impl_->library_handle == nullptr) {
            const char* error = dlerror();
            throw std::runtime_error("Failed to load Hush library: " + library_path +
                                     (error != nullptr ? " (" + std::string(error) + ")" : ""));
        }

        auto load_symbol = [&](const char* name) -> void* {
            dlerror();
            void* symbol = dlsym(impl_->library_handle, name);
            const char* error = dlerror();
            if (error != nullptr || symbol == nullptr) {
                throw std::runtime_error("Missing Hush symbol: " + std::string(name));
            }
            return symbol;
        };

        impl_->model_load_from_path =
            reinterpret_cast<Impl::model_load_from_path_fn>(load_symbol("weya_nc_model_load_from_path"));
        impl_->model_free =
            reinterpret_cast<Impl::model_free_fn>(load_symbol("weya_nc_model_free"));
        impl_->session_create =
            reinterpret_cast<Impl::session_create_fn>(load_symbol("weya_nc_session_create"));
        impl_->session_free =
            reinterpret_cast<Impl::session_free_fn>(load_symbol("weya_nc_session_free"));
        impl_->get_frame_length =
            reinterpret_cast<Impl::get_frame_length_fn>(load_symbol("weya_nc_get_frame_length"));
        impl_->get_sample_rate =
            reinterpret_cast<Impl::get_sample_rate_fn>(load_symbol("weya_nc_get_sample_rate"));
        impl_->process_frame =
            reinterpret_cast<Impl::process_frame_fn>(load_symbol("weya_nc_process_frame"));
        impl_->reset =
            reinterpret_cast<Impl::reset_fn>(load_symbol("weya_nc_reset"));
#endif
        impl_->library_path = library_path;
    }

    if (impl_->model == nullptr) {
        impl_->model = impl_->model_load_from_path(model_path.c_str());
        if (impl_->model == nullptr) {
            throw std::runtime_error("Failed to load Hush model: " + model_path);
        }
        impl_->model_path = model_path;
    }

    if (impl_->session != nullptr) {
        impl_->session_free(impl_->session);
        impl_->session = nullptr;
    }

    impl_->session =
        impl_->session_create(impl_->model, static_cast<std::size_t>(config_.sample_rate),
                              config_.attenuation_limit_db);
    if (impl_->session == nullptr) {
        throw std::runtime_error("Failed to create Hush session.");
    }

    impl_->frame_size = impl_->get_frame_length(impl_->session);
    impl_->internal_sample_rate = static_cast<int>(impl_->get_sample_rate(impl_->session));
    impl_->current_makeup_gain = 1.0f;
    impl_->reset(impl_->session);
#endif
}

std::size_t HushEnhancer::frame_size() const {
#if !defined(CLEANMIC_HAS_HUSH) || !CLEANMIC_HAS_HUSH
    return 0;
#else
    return impl_->frame_size;
#endif
}

int HushEnhancer::internal_sample_rate() const {
#if !defined(CLEANMIC_HAS_HUSH) || !CLEANMIC_HAS_HUSH
    return 0;
#else
    return impl_->internal_sample_rate;
#endif
}

}  // namespace cleanmic::processing
