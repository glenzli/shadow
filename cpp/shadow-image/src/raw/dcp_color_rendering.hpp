#pragma once

#include <shadow/image/dcp_color_development.hpp>

namespace shadow::image::detail {

struct PreparedDcpRenderingStages final {
    std::optional<DcpHsvTable> hue_sat_map;
    std::optional<DcpHsvTable> look_table;
    std::vector<DcpToneCurvePoint> tone_curve;
    std::vector<double> tone_curve_second_derivatives;
};

[[nodiscard]] PreparedDcpRenderingStages prepare_dcp_rendering_stages(
    const DcpProfile& profile,
    double calibration1_weight
);

[[nodiscard]] bool dcp_rendering_stages_valid(
    const DcpColorTransform& transform
) noexcept;

} // namespace shadow::image::detail
