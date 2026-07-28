#pragma once

#include <shadow/image/decoder_types.hpp>
#include <shadow/image/libraw_development_settings.hpp>
#include <shadow/image/raw_development_plan.hpp>
#include <shadow/image/reference_pixels.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace shadow::image {

inline constexpr std::uint32_t libraw_reference_development_contract_version = 2U;

void validate_libraw_development_settings(
    const LibRawDevelopmentSettings& settings
);

[[nodiscard]] std::string compact_libraw_development_settings_identity(
    const LibRawDevelopmentSettings& settings
);

class LibRawReferenceDeveloper final {
public:
    LibRawReferenceDeveloper(
        std::filesystem::path path,
        LibRawDevelopmentSettings settings,
        ProviderInfo provider_info,
        Dimensions source_dimensions,
        bool reference_rgb_available,
        bool raw_frame_available,
        std::uint16_t nikon_nef_compression
    );

    [[nodiscard]] const RawDevelopmentCapabilities& capabilities() const noexcept;

    [[nodiscard]] RawDevelopmentPlanNegotiation negotiate(
        const RawDevelopmentPlan& plan
    ) const noexcept;

    [[nodiscard]] PixelBuffer render(
        const RawDevelopmentPlan& plan
    ) const;

    [[nodiscard]] PixelBuffer render_preview(
        std::uint32_t max_edge,
        const RawDevelopmentPlan& plan
    ) const;

private:
    [[nodiscard]] RawDevelopmentPlanNegotiation require_accepted_plan(
        const RawDevelopmentPlan& plan,
        bool preview_render
    ) const;

    [[nodiscard]] PixelBuffer render_impl(
        bool half_size,
        const RawDevelopmentPlan& requested_plan,
        const RawDevelopmentPlanNegotiation& negotiation,
        std::optional<std::uint32_t> preview_max_edge = std::nullopt
    ) const;

    void require_available() const;

    std::filesystem::path path_;
    LibRawDevelopmentSettings settings_;
    ProviderInfo provider_info_;
    Dimensions source_dimensions_;
    RawDevelopmentCapabilities capabilities_;
    std::uint16_t nikon_nef_compression_ = 0U;
};

} // namespace shadow::image
