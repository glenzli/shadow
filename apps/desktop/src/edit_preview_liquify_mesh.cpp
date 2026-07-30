#include "edit_preview_liquify_mesh.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

[[nodiscard]] bool finite_rect(const QRectF rect) noexcept {
    return std::isfinite(rect.x()) && std::isfinite(rect.y()) && std::isfinite(rect.width())
           && std::isfinite(rect.height()) && rect.width() > 0.0 && rect.height() > 0.0;
}

[[nodiscard]] bool normalized_point(const QPointF point) noexcept {
    return std::isfinite(point.x()) && std::isfinite(point.y()) && point.x() >= 0.0
           && point.x() <= 1.0 && point.y() >= 0.0 && point.y() <= 1.0;
}

[[nodiscard]] bool
normalized_brush(const double radius, const double strength, const double hardness) noexcept {
    return std::isfinite(radius) && radius > 0.0 && radius <= 1.0 && std::isfinite(strength)
           && strength > 0.0 && strength <= 1.0 && std::isfinite(hardness) && hardness >= 0.0
           && hardness <= 1.0;
}

[[nodiscard]] double smootherstep(const double value) noexcept {
    const double bounded = std::clamp(value, 0.0, 1.0);
    return bounded * bounded * bounded * (bounded * (bounded * 6.0 - 15.0) + 10.0);
}

} // namespace

void EditPreviewLiquifyMesh::reset(const QRectF target_rect, const QRectF texture_rect) {
    target_rect_ = {};
    texture_rect_ = {};
    vertices_.clear();
    indices_.clear();
    last_sample_.reset();
    point_count_ = 0U;
    deformed_ = false;
    if (!finite_rect(target_rect) || !finite_rect(texture_rect)) {
        return;
    }

    static_assert(
        (static_cast<std::size_t>(GRID_COLUMNS) + 1U) * (static_cast<std::size_t>(GRID_ROWS) + 1U)
        <= static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max()) + 1U
    );
    target_rect_ = target_rect;
    texture_rect_ = texture_rect;
    const std::size_t column_count = static_cast<std::size_t>(GRID_COLUMNS) + 1U;
    const std::size_t row_count = static_cast<std::size_t>(GRID_ROWS) + 1U;
    vertices_.reserve(column_count * row_count);
    for (std::size_t row = 0U; row < row_count; ++row) {
        const double vertical = static_cast<double>(row) / static_cast<double>(GRID_ROWS);
        for (std::size_t column = 0U; column < column_count; ++column) {
            const double horizontal =
                static_cast<double>(column) / static_cast<double>(GRID_COLUMNS);
            vertices_.push_back(
                EditPreviewLiquifyVertex{
                    .x = static_cast<float>(target_rect.left() + horizontal * target_rect.width()),
                    .y = static_cast<float>(target_rect.top() + vertical * target_rect.height()),
                    .texture_x =
                        static_cast<float>(texture_rect.left() + horizontal * texture_rect.width()),
                    .texture_y =
                        static_cast<float>(texture_rect.top() + vertical * texture_rect.height()),
                }
            );
        }
    }

    indices_.reserve(
        static_cast<std::size_t>(GRID_COLUMNS) * static_cast<std::size_t>(GRID_ROWS) * 6U
    );
    for (std::size_t row = 0U; row < static_cast<std::size_t>(GRID_ROWS); ++row) {
        for (std::size_t column = 0U; column < static_cast<std::size_t>(GRID_COLUMNS); ++column) {
            const auto top_left = static_cast<std::uint16_t>(row * column_count + column);
            const auto top_right = static_cast<std::uint16_t>(top_left + 1U);
            const auto bottom_left = static_cast<std::uint16_t>(top_left + column_count);
            const auto bottom_right = static_cast<std::uint16_t>(bottom_left + 1U);
            indices_.insert(
                indices_.end(),
                {
                    top_left,
                    bottom_left,
                    top_right,
                    top_right,
                    bottom_left,
                    bottom_right,
                }
            );
        }
    }
}

bool EditPreviewLiquifyMesh::appendNormalizedPoint(
    const QPointF point,
    const double pressure,
    const double radius,
    const double strength,
    const double hardness
) {
    if (!valid() || !normalized_point(point) || !std::isfinite(pressure) || pressure < 0.0
        || pressure > 1.0 || !normalized_brush(radius, strength, hardness)) {
        return false;
    }
    const EditPreviewLiquifySample sample{
        .point = point,
        .pressure = pressure,
    };
    if (!last_sample_.has_value()) {
        last_sample_ = sample;
        point_count_ = 1U;
        return true;
    }
    if (last_sample_->point != point) {
        applyPushSegment(*last_sample_, sample, radius, strength, hardness);
    }
    last_sample_ = sample;
    ++point_count_;
    return true;
}

bool EditPreviewLiquifyMesh::valid() const noexcept {
    const std::size_t expected_vertices =
        (static_cast<std::size_t>(GRID_COLUMNS) + 1U) * (static_cast<std::size_t>(GRID_ROWS) + 1U);
    const std::size_t expected_indices =
        static_cast<std::size_t>(GRID_COLUMNS) * static_cast<std::size_t>(GRID_ROWS) * 6U;
    return finite_rect(target_rect_) && finite_rect(texture_rect_)
           && vertices_.size() == expected_vertices && indices_.size() == expected_indices;
}

bool EditPreviewLiquifyMesh::deformed() const noexcept {
    return deformed_;
}

std::size_t EditPreviewLiquifyMesh::pointCount() const noexcept {
    return point_count_;
}

QRectF EditPreviewLiquifyMesh::targetRect() const noexcept {
    return target_rect_;
}

QRectF EditPreviewLiquifyMesh::textureRect() const noexcept {
    return texture_rect_;
}

std::span<const EditPreviewLiquifyVertex> EditPreviewLiquifyMesh::vertices() const noexcept {
    return vertices_;
}

std::span<const std::uint16_t> EditPreviewLiquifyMesh::indices() const noexcept {
    return indices_;
}

void EditPreviewLiquifyMesh::applyPushSegment(
    const EditPreviewLiquifySample from,
    const EditPreviewLiquifySample to,
    const double normalized_radius,
    const double strength,
    const double hardness
) {
    const QPointF from_pixels{
        target_rect_.left() + from.point.x() * target_rect_.width(),
        target_rect_.top() + from.point.y() * target_rect_.height(),
    };
    const QPointF to_pixels{
        target_rect_.left() + to.point.x() * target_rect_.width(),
        target_rect_.top() + to.point.y() * target_rect_.height(),
    };
    const QPointF delta = to_pixels - from_pixels;
    const double length = std::hypot(delta.x(), delta.y());
    if (length == 0.0) {
        return;
    }
    const double radius =
        std::max(0.5, normalized_radius * std::min(target_rect_.width(), target_rect_.height()));
    const double maximum_spacing = std::max(0.5, radius * 0.25);
    const double required_steps = std::max(1.0, std::ceil(length / maximum_spacing));
    const auto step_count = static_cast<std::size_t>(
        std::min(required_steps, static_cast<double>(MAXIMUM_PREVIEW_STAMPS_PER_SEGMENT))
    );
    const double inverse_step_count = 1.0 / static_cast<double>(step_count);
    for (std::size_t step = 1U; step <= step_count; ++step) {
        const double interpolation = static_cast<double>(step) * inverse_step_count;
        const double pressure =
            from.pressure + (to.pressure - from.pressure) * interpolation;
        const QPointF displacement =
            delta * (strength * inverse_step_count * pressure);
        applyStamp(from_pixels + delta * interpolation, displacement, radius, hardness);
    }
    deformed_ = true;
}

void EditPreviewLiquifyMesh::applyStamp(
    const QPointF center,
    const QPointF displacement,
    const double radius,
    const double hardness
) {
    const double radius_squared = radius * radius;
    const std::size_t column_count = static_cast<std::size_t>(GRID_COLUMNS) + 1U;
    for (std::size_t row = 1U; row < static_cast<std::size_t>(GRID_ROWS); ++row) {
        for (std::size_t column = 1U; column < static_cast<std::size_t>(GRID_COLUMNS); ++column) {
            auto& vertex = vertices_[row * column_count + column];
            const double delta_x = static_cast<double>(vertex.x) - center.x();
            const double delta_y = static_cast<double>(vertex.y) - center.y();
            const double distance_squared = delta_x * delta_x + delta_y * delta_y;
            if (distance_squared >= radius_squared) {
                continue;
            }
            const double normalized_distance = std::sqrt(distance_squared) / radius;
            double weight = 1.0;
            if (hardness < 1.0 && normalized_distance > hardness) {
                const double falloff_position = (normalized_distance - hardness) / (1.0 - hardness);
                weight = 1.0 - smootherstep(falloff_position);
            }
            vertex.x = static_cast<float>(std::clamp(
                static_cast<double>(vertex.x) + displacement.x() * weight,
                target_rect_.left(),
                target_rect_.right()
            ));
            vertex.y = static_cast<float>(std::clamp(
                static_cast<double>(vertex.y) + displacement.y() * weight,
                target_rect_.top(),
                target_rect_.bottom()
            ));
        }
    }
}
