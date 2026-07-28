#pragma once

#include <shadow/image/optics.hpp>

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef SHADOW_IMAGE_HAS_LENSFUN
#define SHADOW_IMAGE_HAS_LENSFUN 0
#endif

#if SHADOW_IMAGE_HAS_LENSFUN
#include <lensfun/lensfun.h>
#endif

namespace shadow::image::lensfun_profile_catalog {

struct Match final {
#if SHADOW_IMAGE_HAS_LENSFUN
    const lfCamera* camera = nullptr;
    const lfLens* lens = nullptr;
#endif
    std::string camera_name;
    std::string lens_name;
};

struct Resolution final {
    OpticsProfileStatus status = OpticsProfileStatus::camera_not_found;
    std::optional<Match> match;
};

// Owns the Lensfun database resource, identity matching, compatible-profile projection, and the
// match cache. Pixel correction consumes an immutable match but does not participate in catalog
// discovery or synchronization.
class Catalog final {
public:
    explicit Catalog(std::optional<std::filesystem::path> database_directory);
    Catalog(const Catalog&) = delete;
    Catalog& operator=(const Catalog&) = delete;
    Catalog(Catalog&&) = delete;
    Catalog& operator=(Catalog&&) = delete;
    ~Catalog();

    [[nodiscard]] const OpticsProviderInfo& info() const noexcept;
    [[nodiscard]] bool profile_identity_available(
        const AssetMetadata& metadata,
        const OpticsSettings& settings
    ) const;
    [[nodiscard]] std::vector<OpticsProfileCandidate> profile_candidates(
        const AssetMetadata& metadata
    ) const;
    [[nodiscard]] Resolution resolve_profile(
        const AssetMetadata& metadata,
        const OpticsSettings& settings
    ) const;

private:
#if SHADOW_IMAGE_HAS_LENSFUN
    [[nodiscard]] const lfCamera* find_camera_locked(
        const std::string& make,
        const std::string& model
    ) const;
#endif

    OpticsProviderInfo info_;
#if SHADOW_IMAGE_HAS_LENSFUN
    std::unique_ptr<lfDatabase> database_;
    mutable std::mutex database_mutex_;
    mutable std::unordered_map<std::string, Resolution> match_cache_;
#endif
};

} // namespace shadow::image::lensfun_profile_catalog
