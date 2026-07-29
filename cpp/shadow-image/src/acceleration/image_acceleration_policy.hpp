#pragma once

#include <optional>

namespace shadow::image::detail {

enum class ImageAccelerationPreference {
    automatic,
    cpu,
    metal,
};

// Returns nullopt only for a non-empty unsupported SHADOW_IMAGE_ACCELERATION value. Public
// execution boundaries retain ownership of their domain-specific typed error.
[[nodiscard]] std::optional<ImageAccelerationPreference>
image_acceleration_preference_from_environment() noexcept;

} // namespace shadow::image::detail
