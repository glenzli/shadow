#pragma once

#include "lensfun_modifier_plan.hpp"

#include <shadow/image/decoder_error.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <string>

#ifndef SHADOW_IMAGE_HAS_LENSFUN
#define SHADOW_IMAGE_HAS_LENSFUN 0
#endif

#if SHADOW_IMAGE_HAS_LENSFUN
#include <lensfun/lensfun.h>
#endif

namespace shadow::image::detail::lensfun_modifier_plan {

namespace internal {

inline constexpr std::size_t rgb_channels = 3U;
inline constexpr std::size_t coordinates_per_pixel = rgb_channels * 2U;
// Keep region evidence on the same call boundaries as the full-frame CPU oracle. Stable Lensfun
// advances its coordinate polynomial along x and its vignetting y value across a call in fp32.
// Beginning at a tile or arbitrary row therefore gives a mathematically close but observably
// different result on textured HDR input. Full-width aligned batches make the result independent
// of the requested tile while bounding temporary memory for large images.
inline constexpr std::uint32_t remap_rows_per_batch = 48U;
inline constexpr std::uint32_t vignetting_rows_per_batch = 48U;

#if SHADOW_IMAGE_HAS_LENSFUN

struct CameraDeleter final {
    void operator()(lfCamera* camera) const noexcept;
};

struct LensDeleter final {
    void operator()(lfLens* lens) const noexcept;
};

struct ConfiguredModifier final {
    std::unique_ptr<lfModifier> modifier;
    int flags = 0;
    bool applied_scaling = false;
};

#endif

[[nodiscard]] inline bool finite_positive(const double value) noexcept {
    return std::isfinite(value) && value > 0.0;
}

[[nodiscard]] inline std::size_t checked_sample_count(
    const std::uint32_t width,
    const std::uint32_t height,
    const std::size_t components,
    const char* diagnostic
) {
    const auto width_size = static_cast<std::size_t>(width);
    const auto height_size = static_cast<std::size_t>(height);
    if (width == 0U || height == 0U || components == 0U
        || width_size > std::numeric_limits<std::size_t>::max() / components
        || height_size > std::numeric_limits<std::size_t>::max() / (width_size * components)) {
        throw DecodeError(DecodeErrorCode::resource_limit, 0, diagnostic);
    }
    return width_size * height_size * components;
}

inline void validate_output_rect(const GeometryPixelRect rect, const Dimensions full_dimensions) {
    if (rect.width == 0U || rect.height == 0U || rect.x >= full_dimensions.width
        || rect.y >= full_dimensions.height || rect.width > full_dimensions.width - rect.x
        || rect.height > full_dimensions.height - rect.y
        || rect.width > static_cast<std::uint32_t>(std::numeric_limits<int>::max())
        || rect.height > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "Lensfun region output rectangle is outside the full image"
        );
    }
}

[[nodiscard]] inline bool
coordinate_in_full_image(const float x, const float y, const Dimensions dimensions) noexcept {
    return std::isfinite(x) && std::isfinite(y) && x >= 0.0F && y >= 0.0F
           && x <= static_cast<float>(dimensions.width - 1U)
           && y <= static_cast<float>(dimensions.height - 1U);
}

template <typename Sample> class AlignedSamples final {
  public:
    explicit AlignedSamples(const std::size_t count) {
        if (count == 0U || count > std::numeric_limits<std::size_t>::max() / sizeof(Sample)) {
            throw DecodeError(
                DecodeErrorCode::resource_limit,
                0,
                "Lensfun aligned working buffer is too large"
            );
        }
        data_ = static_cast<Sample*>(::operator new(count * sizeof(Sample), std::align_val_t{16U}));
    }

    AlignedSamples(const AlignedSamples&) = delete;
    AlignedSamples& operator=(const AlignedSamples&) = delete;

    ~AlignedSamples() {
        ::operator delete(data_, std::align_val_t{16U});
    }

    [[nodiscard]] Sample* data() noexcept {
        return data_;
    }

    [[nodiscard]] const Sample* data() const noexcept {
        return data_;
    }

  private:
    Sample* data_ = nullptr;
};

[[noreturn]] void throw_lensfun_unavailable();

} // namespace internal

struct LensfunModifierPlan::Impl final {
    Dimensions full_dimensions;
    AssetMetadata metadata;
    OpticsSettings settings;
    OpticsProviderInfo provider;
    OpticsProfileReceipt receipt;
    std::string camera_profile;
    std::string lens_profile;
#if SHADOW_IMAGE_HAS_LENSFUN
    std::unique_ptr<lfCamera, internal::CameraDeleter> camera;
    std::unique_ptr<lfLens, internal::LensDeleter> lens;
#endif
};

namespace internal {

#if SHADOW_IMAGE_HAS_LENSFUN

[[nodiscard]] ConfiguredModifier
configure_modifier(const LensfunModifierPlan::Impl& plan, lfPixelFormat pixel_format);

[[nodiscard]] OpticsProfileReceipt
receipt_for(const LensfunModifierPlan::Impl& plan, const ConfiguredModifier& modifier);

#endif

} // namespace internal

} // namespace shadow::image::detail::lensfun_modifier_plan
