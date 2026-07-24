#pragma once

#include <shadow/image/camera_profile_catalog.hpp>
#include <shadow/image/raw_development.hpp>

#include <array>
#include <compare>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace shadow::image {

inline constexpr std::uint32_t dcp_color_developer_version = 1U;
inline constexpr std::uint32_t dcp_color_receipt_schema_version = 1U;

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

    [[nodiscard]] bool valid() const noexcept;
    auto operator<=>(const DcpColorDevelopmentReceipt&) const = default;
};

struct DcpColorTransform final {
    // Row-major Camera RGB -> linear sRGB/Rec.709 D65. Camera samples are the unclipped,
    // black-subtracted, white-level-normalized output of Shadow's demosaic stage. White balance,
    // D50/D65 adaptation, and BaselineExposureOffset are already folded into this matrix.
    std::array<double, 9U> camera_to_linear_srgb_d65{};
    DcpColorDevelopmentReceipt receipt;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::array<double, 3U> apply(
        const std::array<double, 3U>& camera_rgb
    ) const noexcept;
};

// Compiles one exact local DCP into an immutable pixel transform. HueSatMap, LookTable, and
// ProfileToneCurve are intentionally all-or-nothing: until their working-space semantics are
// implemented, a profile carrying any of them is rejected rather than partially applied.
//
// The resulting double-precision transform never clips negative or super-white values. Shadow's
// current u16 source boundary may quantize later, but the camera-profile developer itself keeps
// highlight headroom available for a future float RawFrame source cache.
[[nodiscard]] DcpColorTransform compile_dcp_color_transform(
    const CameraProfileDefinition& definition,
    const RawFrameDescriptor& descriptor
);

[[nodiscard]] std::string dcp_color_receipt_identity(
    const DcpColorDevelopmentReceipt& receipt
);

} // namespace shadow::image
