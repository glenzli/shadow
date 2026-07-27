#pragma once

#include <shadow/image/photo_geometry.hpp>
#include <shadow/image/raw_pipeline.hpp>
#include <shadow/image/working_rgb.hpp>

#include <cstddef>

namespace shadow::image::proxy_detail {

[[nodiscard]] std::size_t checked_interleaved_rgb_sample_count(Dimensions dimensions);

[[nodiscard]] Dimensions developed_source_dimensions(
    const DevelopedSourcePixels& source
) noexcept;

void validate_developed_source(const SceneLinearRgbFrame& source);
void validate_developed_source(const DevelopedSourcePixels& source);

[[nodiscard]] FloatRgbImage resize_developed_source_to_working(
    const DevelopedSourcePixels& source,
    Dimensions target
);

[[nodiscard]] FloatRgbImage crop_developed_source_to_working(
    const DevelopedSourcePixels& source,
    GeometryPixelRect rect
);

} // namespace shadow::image::proxy_detail
