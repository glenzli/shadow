#include "lensfun_modifier_plan_internal.hpp"

#include <shadow/image/decoder_error.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace shadow::image::detail::lensfun_modifier_plan {

namespace {

#if SHADOW_IMAGE_HAS_LENSFUN

void compile_full_row_coordinates(
    lfModifier& modifier,
    const Dimensions full_dimensions,
    const GeometryPixelRect output_rect,
    std::vector<float>& absolute_source_coordinates
) {
    const std::uint32_t batch_rows =
        std::min(internal::remap_rows_per_batch, full_dimensions.height);
    const std::size_t coordinate_capacity = internal::checked_sample_count(
        full_dimensions.width,
        batch_rows,
        internal::coordinates_per_pixel,
        "Lensfun full-row region coordinate table is too large"
    );
    internal::AlignedSamples<float> full_row_coordinates(coordinate_capacity);
    const std::uint32_t first_batch_y =
        output_rect.y - output_rect.y % internal::remap_rows_per_batch;
    const std::uint32_t output_end_y = output_rect.y + output_rect.height;
    for (std::uint32_t batch_y = first_batch_y; batch_y < output_end_y;
         batch_y += internal::remap_rows_per_batch) {
        const std::uint32_t rows =
            std::min(internal::remap_rows_per_batch, full_dimensions.height - batch_y);
        if (!modifier.ApplySubpixelGeometryDistortion(
                0.0F,
                static_cast<float>(batch_y),
                static_cast<int>(full_dimensions.width),
                static_cast<int>(rows),
                full_row_coordinates.data()
            )) {
            throw DecodeError(
                DecodeErrorCode::internal,
                0,
                "Lensfun unexpectedly declined an enabled full-row region remap"
            );
        }

        const std::uint32_t first_output_y = std::max(batch_y, output_rect.y);
        const std::uint32_t last_output_y = std::min(batch_y + rows, output_end_y);
        for (std::uint32_t output_y = first_output_y; output_y < last_output_y; ++output_y) {
            const std::size_t source_pixel =
                (static_cast<std::size_t>(output_y - batch_y) * full_dimensions.width
                 + output_rect.x)
                * internal::coordinates_per_pixel;
            const std::size_t destination_pixel = static_cast<std::size_t>(output_y - output_rect.y)
                                                  * output_rect.width
                                                  * internal::coordinates_per_pixel;
            std::copy_n(
                full_row_coordinates.data() + static_cast<std::ptrdiff_t>(source_pixel),
                static_cast<std::size_t>(output_rect.width) * internal::coordinates_per_pixel,
                absolute_source_coordinates.begin() + static_cast<std::ptrdiff_t>(destination_pixel)
            );
        }
    }
}

void compile_full_row_vignetting_gains(
    lfModifier& modifier,
    const Dimensions full_dimensions,
    const GeometryPixelRect source,
    std::vector<float>& profile_vignetting_gains
) {
    const std::uint32_t batch_rows =
        std::min(internal::vignetting_rows_per_batch, full_dimensions.height);
    const std::size_t batch_sample_capacity = internal::checked_sample_count(
        full_dimensions.width,
        batch_rows,
        internal::rgb_channels,
        "Lensfun full-row vignetting batch is too large"
    );
    internal::AlignedSamples<float> full_row_gains(batch_sample_capacity);
    const std::size_t full_row_sample_count =
        static_cast<std::size_t>(full_dimensions.width) * internal::rgb_channels;
    const std::size_t source_row_sample_count =
        static_cast<std::size_t>(source.width) * internal::rgb_channels;
    const std::uint32_t first_batch_y = source.y - source.y % internal::vignetting_rows_per_batch;
    const std::uint32_t source_end_y = source.y + source.height;
    for (std::uint32_t batch_y = first_batch_y; batch_y < source_end_y;
         batch_y += internal::vignetting_rows_per_batch) {
        const std::uint32_t rows =
            std::min(internal::vignetting_rows_per_batch, full_dimensions.height - batch_y);
        const std::size_t batch_sample_count =
            static_cast<std::size_t>(full_dimensions.width) * rows * internal::rgb_channels;
        std::fill(
            full_row_gains.data(),
            full_row_gains.data() + static_cast<std::ptrdiff_t>(batch_sample_count),
            1.0F
        );
        if (!modifier.ApplyColorModification(
                full_row_gains.data(),
                0.0F,
                static_cast<float>(batch_y),
                static_cast<int>(full_dimensions.width),
                static_cast<int>(rows),
                LF_CR_3(RED, GREEN, BLUE),
                static_cast<int>(full_row_sample_count * sizeof(float))
            )) {
            throw DecodeError(
                DecodeErrorCode::internal,
                0,
                "Lensfun unexpectedly declined an enabled full-row vignetting correction"
            );
        }
        const std::uint32_t first_source_y = std::max(batch_y, source.y);
        const std::uint32_t last_source_y = std::min(batch_y + rows, source_end_y);
        for (std::uint32_t source_y = first_source_y; source_y < last_source_y; ++source_y) {
            const std::size_t source_sample =
                static_cast<std::size_t>(source_y - batch_y) * full_row_sample_count
                + static_cast<std::size_t>(source.x) * internal::rgb_channels;
            const std::size_t destination_sample =
                static_cast<std::size_t>(source_y - source.y) * source_row_sample_count;
            std::copy_n(
                full_row_gains.data() + static_cast<std::ptrdiff_t>(source_sample),
                source_row_sample_count,
                profile_vignetting_gains.begin()
                    + static_cast<std::ptrdiff_t>(destination_sample)
            );
        }
    }
}

#endif

} // namespace

PreparedRegion LensfunModifierPlan::prepare_region(const GeometryPixelRect output_rect) const {
#if SHADOW_IMAGE_HAS_LENSFUN
    internal::validate_output_rect(output_rect, implementation_->full_dimensions);
    internal::ConfiguredModifier configured =
        internal::configure_modifier(*implementation_, LF_PF_F32);
    const bool coordinate_remap =
        implementation_->receipt.applied_distortion || implementation_->receipt.applied_tca;
    const bool profile_vignetting = implementation_->receipt.applied_vignetting;
    std::optional<GeometryPixelRect> source_preimage;
    std::vector<float> absolute_source_coordinates;
    std::vector<float> profile_vignetting_gains;

    if (coordinate_remap) {
        const std::size_t coordinate_count = internal::checked_sample_count(
            output_rect.width,
            output_rect.height,
            internal::coordinates_per_pixel,
            "Lensfun region coordinate table is too large"
        );
        absolute_source_coordinates.resize(coordinate_count);
        compile_full_row_coordinates(
            *configured.modifier,
            implementation_->full_dimensions,
            output_rect,
            absolute_source_coordinates
        );

        std::uint32_t minimum_x = std::numeric_limits<std::uint32_t>::max();
        std::uint32_t minimum_y = std::numeric_limits<std::uint32_t>::max();
        std::uint32_t maximum_x = 0U;
        std::uint32_t maximum_y = 0U;
        bool found_in_bounds = false;
        for (std::size_t index = 0U; index < coordinate_count; index += 2U) {
            const float source_x = absolute_source_coordinates[index];
            const float source_y = absolute_source_coordinates[index + 1U];
            if (!internal::coordinate_in_full_image(
                    source_x,
                    source_y,
                    implementation_->full_dimensions
                )) {
                continue;
            }
            const auto x0 = static_cast<std::uint32_t>(std::floor(source_x));
            const auto y0 = static_cast<std::uint32_t>(std::floor(source_y));
            const std::uint32_t x1 = std::min(x0 + 1U, implementation_->full_dimensions.width - 1U);
            const std::uint32_t y1 =
                std::min(y0 + 1U, implementation_->full_dimensions.height - 1U);
            minimum_x = std::min(minimum_x, x0);
            minimum_y = std::min(minimum_y, y0);
            maximum_x = std::max(maximum_x, x1);
            maximum_y = std::max(maximum_y, y1);
            found_in_bounds = true;
        }
        if (found_in_bounds) {
            source_preimage = GeometryPixelRect{
                .x = minimum_x,
                .y = minimum_y,
                .width = maximum_x - minimum_x + 1U,
                .height = maximum_y - minimum_y + 1U,
            };
        }
    } else {
        source_preimage = output_rect;
    }

    if (profile_vignetting && source_preimage.has_value()) {
        const GeometryPixelRect source = *source_preimage;
        const std::size_t gain_count = internal::checked_sample_count(
            source.width,
            source.height,
            internal::rgb_channels,
            "Lensfun region vignetting gain table is too large"
        );
        profile_vignetting_gains.resize(gain_count);
        compile_full_row_vignetting_gains(
            *configured.modifier,
            implementation_->full_dimensions,
            source,
            profile_vignetting_gains
        );
    }
    return PreparedRegion(
        output_rect,
        std::move(source_preimage),
        std::move(absolute_source_coordinates),
        std::move(profile_vignetting_gains),
        implementation_->settings,
        coordinate_remap,
        profile_vignetting,
        region_plan_identity_
    );
#else
    static_cast<void>(output_rect);
    internal::throw_lensfun_unavailable();
#endif
}

} // namespace shadow::image::detail::lensfun_modifier_plan
