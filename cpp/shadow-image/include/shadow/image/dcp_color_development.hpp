#pragma once

#include <shadow/image/camera_profile_catalog.hpp>
#include <shadow/image/raw_development.hpp>

#include <array>
#include <compare>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace shadow::image {

inline constexpr std::uint32_t dcp_color_developer_version = 1U;
inline constexpr std::uint32_t dcp_color_receipt_schema_version = 1U;

// CPU remains the numerical reference for DCP input rendering.  Metal is an equivalent fp32
// executor for the scene-linear RAW path; keeping the effective executor visible in the RAW
// development signature prevents a cached preview from silently changing its rendering backend.
enum class DcpColorExecutionBackend : std::uint8_t {
    cpu,
    metal,
};

[[nodiscard]] std::string_view dcp_color_execution_backend_identity(
    DcpColorExecutionBackend backend
) noexcept;

enum class DcpMatrixRoute : std::uint8_t {
    forward_matrix,
    inverse_color_matrix,
};

enum class DcpColorDevelopmentErrorCode : std::uint8_t {
    invalid_input,
    unsupported_rendering_feature,
    unsupported_illuminant,
    singular_matrix,
    invalid_white_point,
};

class DcpColorDevelopmentError final : public std::invalid_argument {
public:
    DcpColorDevelopmentError(DcpColorDevelopmentErrorCode code, std::string message);

    [[nodiscard]] DcpColorDevelopmentErrorCode code() const noexcept;

private:
    DcpColorDevelopmentErrorCode code_;
};

struct DcpColorDevelopmentReceipt final {
    std::uint32_t schema_version = dcp_color_receipt_schema_version;
    std::uint32_t developer_version = dcp_color_developer_version;
    std::string profile_content_identity;
    std::string normalized_camera_model;
    DcpMatrixRoute matrix_route = DcpMatrixRoute::inverse_color_matrix;
    std::uint16_t calibration_illuminant1 = 0U;
    std::uint16_t calibration_illuminant2 = 0U;
    // Weight of calibration 1. Single-illuminant profiles always record 1.
    double calibration1_weight = 1.0;
    double estimated_white_x = 0.0;
    double estimated_white_y = 0.0;
    double estimated_correlated_color_temperature = 0.0;
    double baseline_exposure_offset_ev = 0.0;
    // These stages are part of input rendering, not a creative node.  Keeping
    // them in the receipt makes a camera-profile result auditable and prevents
    // a cache key from claiming a look that the renderer did not apply.
    bool hue_sat_map_applied = false;
    bool look_table_applied = false;
    bool tone_curve_applied = false;

    [[nodiscard]] bool valid() const noexcept;
    auto operator<=>(const DcpColorDevelopmentReceipt&) const = default;
};

struct DcpColorTransform final {
    // Row-major Camera RGB -> linear sRGB/Rec.709 D65. Camera samples are the unclipped,
    // black-subtracted, white-level-normalized output of Shadow's demosaic stage. White balance,
    // D50/D65 adaptation, and BaselineExposureOffset are already folded into this matrix.
    std::array<double, 9U> camera_to_linear_srgb_d65{};
    // DCP's optional non-matrix rendering stages run after this primary
    // transform in the DNG-defined linear ProPhoto working space.  They are
    // compiled with the profile rather than represented as user-editable
    // Recipe operations, because they describe the camera input rendering.
    std::optional<DcpHsvTable> hue_sat_map;
    std::optional<DcpHsvTable> look_table;
    std::vector<DcpToneCurvePoint> tone_curve;
    std::vector<double> tone_curve_second_derivatives;
    DcpColorDevelopmentReceipt receipt;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::array<double, 3U> apply(
        const std::array<double, 3U>& camera_rgb
    ) const noexcept;

    [[nodiscard]] bool has_post_matrix_stages() const noexcept;
};

// Compiles one local DCP into an immutable input-rendering transform. The camera matrix,
// HueSatMap, LookTable, and ProfileToneCurve are deliberately separate from Recipe nodes:
// they establish the photo's camera rendering before all user adjustments.
//
// The resulting double-precision transform never clips negative or super-white values. Shadow's
// owned RawFrame route applies the optional post-matrix stages directly to its fp32 scene-linear
// buffer, so camera-profile rendering does not discard highlight headroom before the edit graph.
[[nodiscard]] DcpColorTransform compile_dcp_color_transform(
    const CameraProfileDefinition& definition,
    const RawFrameDescriptor& descriptor
);

// Applies the compiled DCP HSV/LUT/tone stages to a canonical linear-sRGB RAW output.  This is
// intentionally separate from the fused Bayer developer: it keeps the hot provider-neutral
// demosaic path focused on sensor reconstruction while preserving one explicit DCP working-space
// boundary.  The fp32 overload is the owned RAW route and preserves scene-linear headroom; the
// packed u16 overload exists only for compatibility providers that already have a bounded source.
[[nodiscard]] DcpColorExecutionBackend apply_dcp_color_rendering_stages(
    SceneLinearRgbFrame& pixels,
    const DcpColorTransform& transform
);

[[nodiscard]] DcpColorExecutionBackend apply_dcp_color_rendering_stages(
    PixelBuffer& pixels,
    const DcpColorTransform& transform
);

[[nodiscard]] std::string dcp_color_receipt_identity(
    const DcpColorDevelopmentReceipt& receipt
);

} // namespace shadow::image
