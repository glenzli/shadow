#pragma once

#include <array>
#include <cstddef>

namespace shadow::image {

struct AdjustmentNode;
struct Chromaticity;
struct RgbWhiteBalanceAdjustment;
struct WorkingRgbSpace;

namespace detail {

using Vector3 = std::array<double, 3U>;
using Matrix3 = std::array<Vector3, 3U>;

struct WorkingSpaceTransform final {
    Matrix3 rgb_to_xyz{};
    Matrix3 xyz_to_rgb{};
};

[[nodiscard]] bool finite_chromaticity(const Chromaticity& value) noexcept;

[[nodiscard]] Vector3 apply_color_matrix(const Matrix3& matrix, const Vector3& vector) noexcept;

[[nodiscard]] WorkingSpaceTransform prepare_working_space_transform(
    const WorkingRgbSpace& space,
    const AdjustmentNode& node,
    std::size_t index
);

// Local-mask selection has no AdjustmentNode of its own. This overload keeps a
// working-space failure attached to the layer boundary instead of inventing a
// misleading node index.
[[nodiscard]] WorkingSpaceTransform prepare_working_space_transform(const WorkingRgbSpace& space);

[[nodiscard]] Matrix3 prepare_rgb_white_balance_matrix(
    const WorkingRgbSpace& space,
    const RgbWhiteBalanceAdjustment& parameters,
    const AdjustmentNode& node,
    std::size_t index
);

[[nodiscard]] Vector3
working_rgb_to_oklab(const WorkingSpaceTransform& transform, const Vector3& rgb) noexcept;

[[nodiscard]] Vector3
oklab_to_working_rgb(const WorkingSpaceTransform& transform, const Vector3& lab) noexcept;

} // namespace detail

} // namespace shadow::image
