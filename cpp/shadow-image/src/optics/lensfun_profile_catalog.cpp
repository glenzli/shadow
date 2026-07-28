#include "lensfun_profile_catalog.hpp"

#include <algorithm>
#include <string_view>
#include <tuple>
#include <utility>

namespace shadow::image::lensfun_profile_catalog {

namespace {

[[nodiscard]] std::string trim_ascii(const std::string_view value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) {
        return {};
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(begin, end - begin + 1U));
}

#if SHADOW_IMAGE_HAS_LENSFUN

[[nodiscard]] std::string profile_match_key(
    const AssetMetadata& metadata,
    const OpticsSettings& settings
) {
    // These fields determine profile discovery only. Focal/aperture/distance are deliberately
    // absent: Lensfun interpolates calibration in the per-render modifier, not the match cache.
    return trim_ascii(metadata.make) + '\x1f' + trim_ascii(metadata.model) + '\x1f'
        + trim_ascii(metadata.normalized_make) + '\x1f' + trim_ascii(metadata.normalized_model)
        + '\x1f' + trim_ascii(metadata.lens_make) + '\x1f' + trim_ascii(metadata.lens_model)
        + '\x1f' + trim_ascii(settings.camera_profile_maker) + '\x1f'
        + trim_ascii(settings.camera_profile_model) + '\x1f'
        + trim_ascii(settings.lens_profile_maker) + '\x1f'
        + trim_ascii(settings.lens_profile_model);
}

[[nodiscard]] std::string lensfun_name(const lfMLstr value) {
    return value == nullptr ? std::string{} : std::string(lf_mlstr_get(value));
}

#endif

} // namespace

Catalog::Catalog(std::optional<std::filesystem::path> database_directory) {
    info_.id = "lensfun";
#if SHADOW_IMAGE_HAS_LENSFUN
    database_ = std::make_unique<lfDatabase>();
    bool loaded = false;
    if (database_directory.has_value()) {
#if LF_VERSION_MICRO >= 99
        loaded = database_->Load(database_directory->string().c_str()) == LF_NO_ERROR;
#else
        // Stable Lensfun 0.3.4's Load(path) reads one XML file; directory loading is a
        // separate API. The 0.3.99 development line folds directory discovery into Load().
        loaded = database_->LoadDirectory(database_directory->string().c_str());
#endif
    } else {
        loaded = database_->Load() == LF_NO_ERROR;
    }
    if (!loaded) {
        database_.reset();
        info_.version = "unavailable";
        return;
    }
    const auto directory = database_directory.has_value()
        ? database_directory->string()
        : std::string{"system"};
    info_.version = "lensfun-" + std::to_string(LF_VERSION_MAJOR) + "."
        + std::to_string(LF_VERSION_MINOR) + "." + std::to_string(LF_VERSION_MICRO)
        + ";database=" + directory;
    info_.available = true;
#else
    static_cast<void>(database_directory);
    info_.version = "not-linked";
#endif
}

Catalog::~Catalog() = default;

const OpticsProviderInfo& Catalog::info() const noexcept {
    return info_;
}

bool Catalog::profile_identity_available(
    const AssetMetadata& metadata,
    const OpticsSettings& settings
) const {
    if (!trim_ascii(settings.camera_profile_model).empty()) {
        return true;
    }
    return !trim_ascii(metadata.lens_model).empty()
        && (!trim_ascii(metadata.make).empty()
            || !trim_ascii(metadata.normalized_make).empty())
        && (!trim_ascii(metadata.model).empty()
            || !trim_ascii(metadata.normalized_model).empty());
}

#if SHADOW_IMAGE_HAS_LENSFUN

const lfCamera* Catalog::find_camera_locked(
    const std::string& make,
    const std::string& model
) const {
    if (make.empty() || model.empty()) {
        return nullptr;
    }
    const auto* matches = database_->FindCameras(make.c_str(), model.c_str());
    if (matches == nullptr || matches[0] == nullptr) {
        if (matches != nullptr) {
            lf_free(const_cast<lfCamera**>(matches));
        }
        matches = database_->FindCamerasExt(
            make.c_str(), model.c_str(), LF_SEARCH_LOOSE
        );
    }
    const auto* match = matches != nullptr ? matches[0] : nullptr;
    if (matches != nullptr) {
        lf_free(const_cast<lfCamera**>(matches));
    }
    return match;
}

#endif

std::vector<OpticsProfileCandidate> Catalog::profile_candidates(
    const AssetMetadata& metadata
) const {
#if SHADOW_IMAGE_HAS_LENSFUN
    if (!info_.available || database_ == nullptr) {
        return {};
    }
    std::lock_guard lock(database_mutex_);
    const lfCamera* camera = find_camera_locked(
        trim_ascii(metadata.make), trim_ascii(metadata.model)
    );
    if (camera == nullptr) {
        camera = find_camera_locked(
            trim_ascii(metadata.normalized_make), trim_ascii(metadata.normalized_model)
        );
    }
    if (camera == nullptr) {
        return {};
    }

    // FindLenses(camera, maker, model) parses a human-readable model string and does not
    // treat a null model as "all compatible lenses". Build the structural query used by
    // Lensfun's second overload instead: the camera crop factor is the upper calibration
    // bound and its mount limits the result to profiles that can actually be mounted.
    lfLens compatible_lens;
    compatible_lens.CropFactor = camera->CropFactor;
    if (camera->Mount != nullptr && camera->Mount[0] != '\0') {
        compatible_lens.AddMount(camera->Mount);
    }
    const auto* lenses = database_->FindLenses(&compatible_lens);
    if (lenses == nullptr) {
        return {};
    }
    std::vector<OpticsProfileCandidate> result;
    constexpr std::size_t maximum_candidates = 2'048U;
    for (std::size_t index = 0U;
         lenses[index] != nullptr && result.size() < maximum_candidates;
         ++index) {
        const auto* lens = lenses[index];
        const auto camera_maker = lensfun_name(camera->Maker);
        const auto camera_model = lensfun_name(camera->Model);
        const auto lens_maker = lensfun_name(lens->Maker);
        const auto lens_model = lensfun_name(lens->Model);
        if (camera_model.empty() || lens_model.empty()) {
            continue;
        }
        result.push_back({camera_maker, camera_model, lens_maker, lens_model});
    }
    lf_free(const_cast<lfLens**>(lenses));
    std::ranges::sort(result, [](const auto& left, const auto& right) {
        return std::tie(left.lens_maker, left.lens_model)
            < std::tie(right.lens_maker, right.lens_model);
    });
    result.erase(
        std::unique(result.begin(), result.end(), [](const auto& left, const auto& right) {
            return left.camera_maker == right.camera_maker
                && left.camera_model == right.camera_model
                && left.lens_maker == right.lens_maker
                && left.lens_model == right.lens_model;
        }),
        result.end()
    );
    return result;
#else
    static_cast<void>(metadata);
    return {};
#endif
}

Resolution Catalog::resolve_profile(
    const AssetMetadata& metadata,
    const OpticsSettings& settings
) const {
#if SHADOW_IMAGE_HAS_LENSFUN
    if (!info_.available || database_ == nullptr) {
        return {.status = OpticsProfileStatus::provider_unavailable};
    }
    std::lock_guard lock(database_mutex_);
    const auto cache_key = profile_match_key(metadata, settings);
    if (const auto cached = match_cache_.find(cache_key); cached != match_cache_.end()) {
        return cached->second;
    }
    const auto remember = [&](Resolution result) {
        match_cache_.insert_or_assign(cache_key, result);
        return result;
    };
    const bool manual = !trim_ascii(settings.camera_profile_model).empty();
    const auto camera = [&] {
        if (manual) {
            return find_camera_locked(
                trim_ascii(settings.camera_profile_maker),
                trim_ascii(settings.camera_profile_model)
            );
        }
        if (const auto* exact = find_camera_locked(
                trim_ascii(metadata.make), trim_ascii(metadata.model)
            )) {
            return exact;
        }
        return find_camera_locked(
            trim_ascii(metadata.normalized_make), trim_ascii(metadata.normalized_model)
        );
    }();
    if (camera == nullptr) {
        return remember({.status = OpticsProfileStatus::camera_not_found});
    }
    const auto lens_make = trim_ascii(
        manual ? settings.lens_profile_maker : metadata.lens_make
    );
    const auto lens_model = trim_ascii(
        manual ? settings.lens_profile_model : metadata.lens_model
    );
    const auto* lenses = database_->FindLenses(
        camera,
        lens_make.empty() ? nullptr : lens_make.c_str(),
        lens_model.c_str()
    );
    if (lenses == nullptr || lenses[0] == nullptr) {
        if (lenses != nullptr) {
            lf_free(const_cast<lfLens**>(lenses));
        }
        return remember({.status = OpticsProfileStatus::lens_not_found});
    }
    const auto* lens = lenses[0];
    lf_free(const_cast<lfLens**>(lenses));
    return remember(Resolution{
        .status = OpticsProfileStatus::matched,
        .match = Match{
            .camera = camera,
            .lens = lens,
            .camera_name = lensfun_name(camera->Model),
            .lens_name = lensfun_name(lens->Model),
        },
    });
#else
    static_cast<void>(metadata);
    static_cast<void>(settings);
    return {.status = OpticsProfileStatus::provider_unavailable};
#endif
}

} // namespace shadow::image::lensfun_profile_catalog
