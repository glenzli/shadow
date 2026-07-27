#pragma once

#include <shadow/image/optics.hpp>
#include <shadow/image/reference_pixels.hpp>

#include <algorithm>
#include <cstddef>
#include <utility>

namespace shadow::image::test_support {

class ReplacingOpticsProvider final : public OpticsProvider {
public:
    [[nodiscard]] const OpticsProviderInfo& info() const noexcept override {
        return info_;
    }

    [[nodiscard]] OpticsCorrectionResult correct_reference_rgb(
        const PixelBuffer& input,
        const AssetMetadata&,
        const OpticsSettings& settings
    ) const override {
        ++correction_count_;
        last_settings_ = settings;
        auto corrected = input;
        std::fill(corrected.samples.begin(), corrected.samples.end(), 0U);
        // A third-party implementation may allocate or copy only raster pixels.
        // Preparation owns the source-development receipt and must restore it.
        corrected.raw_development_receipt = {};
        return {
            .receipt =
                {
                    .status = OpticsProfileStatus::matched,
                    .provider_id = "fake-optics",
                    .provider_version = "test-v1",
                    .camera_profile = "Test camera",
                    .lens_profile = "Test lens",
                    .distortion_available = true,
                    .applied_distortion = true,
                },
            .corrected_reference_rgb = std::move(corrected),
        };
    }

    [[nodiscard]] std::size_t correction_count() const noexcept {
        return correction_count_;
    }

    [[nodiscard]] const OpticsSettings& last_settings() const noexcept {
        return last_settings_;
    }

private:
    OpticsProviderInfo info_{
        .id = "fake-optics",
        .version = "test-v1",
        .available = true,
    };
    mutable std::size_t correction_count_ = 0U;
    mutable OpticsSettings last_settings_;
};

} // namespace shadow::image::test_support
