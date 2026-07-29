#pragma once

#include <shadow/image/photo_geometry.hpp>
#include <shadow/image/raw_pipeline.hpp>
#include <shadow/image/working_rgb.hpp>

#include <cstddef>

namespace shadow::image::proxy_detail {

[[nodiscard]] std::size_t checked_interleaved_rgb_sample_count(Dimensions dimensions);

[[nodiscard]] Dimensions developed_source_dimensions(const DevelopedSourcePixels& source) noexcept;

void validate_developed_source(const SceneLinearRgbFrame& source);
void validate_developed_source(const DevelopedSourcePixels& source);

[[nodiscard]] FloatRgbImage
resize_developed_source_to_working(const DevelopedSourcePixels& source, Dimensions target);

[[nodiscard]] FloatRgbImage
crop_developed_source_to_working(const DevelopedSourcePixels& source, GeometryPixelRect rect);

// Moves one already-developed scene-linear region into the common edit working contract without
// copying its fp32 allocation. The full dimensions remain explicit so later masks, noise, and
// geometry use level-zero coordinates rather than treating the region as a miniature image.
[[nodiscard]] FloatRgbImage
take_scene_linear_region_to_working(SceneLinearRgbFrame region, Dimensions full_dimensions);

} // namespace shadow::image::proxy_detail
