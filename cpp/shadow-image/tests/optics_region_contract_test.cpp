#include "../src/optics/scene_linear_region_optics.hpp"
#include "contract_test_assertions.hpp"
#include "scoped_environment.hpp"

#include <shadow/image/optics.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>
#include <vector>

namespace {

namespace image = shadow::image;
using image::test_support::expect;
using image::test_support::failures;
using image::test_support::ScopedEnvironment;

static_assert(
    !std::is_default_constructible_v<image::detail::PreparedRegionOpticsSourceIdentity>,
    "only one prepared RAW source may create its unforgeable optics binding identity"
);

[[nodiscard]] image::SceneLinearRgbFrame source_frame() {
    image::SceneLinearRgbFrame result;
    result.dimensions = {8U, 6U};
    result.row_stride_bytes = 8U * 3U * sizeof(float);
    result.samples.resize(8U * 6U * 3U);
    for (std::uint32_t y = 0U; y < 6U; ++y) {
        for (std::uint32_t x = 0U; x < 8U; ++x) {
            const std::size_t index = (static_cast<std::size_t>(y) * 8U + x) * 3U;
            result.samples[index] = 0.1F + static_cast<float>(x) * 0.03F;
            result.samples[index + 1U] = 0.2F + static_cast<float>(y) * 0.04F;
            result.samples[index + 2U] = 0.3F + static_cast<float>(x + y) * 0.01F;
        }
    }
    return result;
}

[[nodiscard]] image::SceneLinearRgbFrame
crop(const image::SceneLinearRgbFrame& source, const image::GeometryPixelRect rect) {
    image::SceneLinearRgbFrame result;
    result.dimensions = {rect.width, rect.height};
    result.row_stride_bytes = static_cast<std::size_t>(rect.width) * 3U * sizeof(float);
    result.samples.resize(static_cast<std::size_t>(rect.width) * rect.height * 3U);
    for (std::uint32_t y = 0U; y < rect.height; ++y) {
        for (std::uint32_t x = 0U; x < rect.width; ++x) {
            const std::size_t source_index =
                (static_cast<std::size_t>(rect.y + y) * source.dimensions.width + rect.x + x) * 3U;
            const std::size_t output_index = (static_cast<std::size_t>(y) * rect.width + x) * 3U;
            for (std::size_t channel = 0U; channel < 3U; ++channel) {
                result.samples[output_index + channel] = source.samples[source_index + channel];
            }
        }
    }
    return result;
}

class UnknownOpticsProvider final : public image::OpticsProvider {
  public:
    UnknownOpticsProvider() {
        info_.id = "unknown-test";
        info_.version = "v1";
        info_.available = true;
    }

    [[nodiscard]] const image::OpticsProviderInfo& info() const noexcept override {
        return info_;
    }

    [[nodiscard]] image::OpticsCorrectionResult correct_reference_rgb(
        const image::PixelBuffer&,
        const image::AssetMetadata&,
        const image::OpticsSettings&
    ) const override {
        return {};
    }

  private:
    image::OpticsProviderInfo info_;
};

void pointwise_manual_vignette_matches_full_optics() {
    const ScopedEnvironment acceleration("SHADOW_IMAGE_ACCELERATION", "cpu");
    auto provider = image::make_lensfun_optics_provider();
    image::OpticsSettings settings = image::default_optics_settings();
    // Disabling the profile leaves the manual residual active, matching the existing full-image
    // contract while making locality independently provable.
    settings.enabled = false;
    settings.manual_vignetting_amount = 55;
    settings.manual_vignetting_midpoint = 35U;
    const image::AssetMetadata metadata;
    auto full = source_frame();
    const auto corrected = provider->correct_scene_linear_reference(full, metadata, settings);
    const auto plan = image::detail::prepare_scene_linear_region_optics(
        provider.get(),
        full.dimensions,
        metadata,
        settings
    );
    const image::GeometryPixelRect rect{2U, 1U, 4U, 3U};
    auto region = crop(full, rect);
    image::detail::apply_scene_linear_region_optics(region, rect, plan);

    expect(
        plan.kind() == image::detail::SceneLinearRegionOpticsKind::pointwise
            && corrected.corrected_scene_linear_rgb.has_value(),
        "manual vignette compiles into an owned pointwise region plan"
    );
    if (corrected.corrected_scene_linear_rgb.has_value()) {
        expect(
            region.samples == crop(*corrected.corrected_scene_linear_rgb, rect).samples,
            "pointwise region optics uses full-image coordinates and is byte-exact "
            "with CPU full-image optics"
        );
    }
    expect(
        plan.receipt().status == image::OpticsProfileStatus::disabled
            && plan.receipt().provider_id == corrected.receipt.provider_id
            && plan.receipt().provider_version == corrected.receipt.provider_version,
        "pointwise preparation preserves the immediate full-image optics receipt"
    );
}

void nonlocal_and_unknown_providers_fail_closed() {
    auto provider = image::make_lensfun_optics_provider();
    image::OpticsSettings geometry = image::default_optics_settings();
    geometry.enabled = false;
    geometry.manual_distortion = 10;
    const auto remap = image::detail::prepare_scene_linear_region_optics(
        provider.get(),
        {8U, 6U},
        image::AssetMetadata{},
        geometry
    );
    UnknownOpticsProvider unknown;
    const auto materialized = image::detail::prepare_scene_linear_region_optics(
        &unknown,
        {8U, 6U},
        image::AssetMetadata{},
        image::default_optics_settings()
    );
    const auto neutral = image::detail::prepare_scene_linear_region_optics(
        nullptr,
        {8U, 6U},
        image::AssetMetadata{},
        image::default_optics_settings()
    );
    expect(
        remap.kind() == image::detail::SceneLinearRegionOpticsKind::coordinate_remap
            && !remap.resident_eligible(),
        "manual distortion is classified as a coordinate remap rather than a crop-local effect"
    );
    expect(
        materialized.kind() == image::detail::SceneLinearRegionOpticsKind::materialized_only
            && !materialized.resident_eligible(),
        "an unknown optics provider cannot opt into resident regions by accident"
    );
    expect(
        neutral.kind() == image::detail::SceneLinearRegionOpticsKind::neutral
            && neutral.resident_eligible()
            && neutral.receipt().status == image::OpticsProfileStatus::disabled,
        "the no-provider path is explicitly neutral and resident-safe"
    );
}

} // namespace

int main() {
    pointwise_manual_vignette_matches_full_optics();
    nonlocal_and_unknown_providers_fail_closed();
    return failures == 0 ? 0 : 1;
}
