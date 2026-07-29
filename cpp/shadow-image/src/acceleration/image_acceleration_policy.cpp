#include "image_acceleration_policy.hpp"

#include <cstdlib>
#include <string_view>

namespace shadow::image::detail {

std::optional<ImageAccelerationPreference>
image_acceleration_preference_from_environment() noexcept {
    const char* configured = std::getenv("SHADOW_IMAGE_ACCELERATION");
    if (configured == nullptr || *configured == '\0' || std::string_view(configured) == "auto") {
        return ImageAccelerationPreference::automatic;
    }
    if (std::string_view(configured) == "cpu") {
        return ImageAccelerationPreference::cpu;
    }
    if (std::string_view(configured) == "metal") {
        return ImageAccelerationPreference::metal;
    }
    return std::nullopt;
}

} // namespace shadow::image::detail
