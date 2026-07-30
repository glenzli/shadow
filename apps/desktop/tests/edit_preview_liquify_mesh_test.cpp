#include "edit_preview_liquify_mesh.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "edit preview Liquify mesh failed: " << message << '\n';
    }
    return condition;
}

[[nodiscard]] std::size_t vertex_index(const std::size_t column, const std::size_t row) noexcept {
    return row * (static_cast<std::size_t>(EditPreviewLiquifyMesh::GRID_COLUMNS) + 1U) + column;
}

} // namespace

int main() {
    EditPreviewLiquifyMesh mesh;
    mesh.reset(QRectF{10.0, 20.0, 512.0, 384.0}, QRectF{0.0, 0.0, 1.0, 1.0});
    if (!require(mesh.valid(), "a finite non-empty rectangle creates a valid mesh")
        || !require(
            mesh.vertices().size() == 16'641U && mesh.indices().size() == 98'304U,
            "the fixed grid remains bounded and uses 16-bit indices"
        )) {
        return EXIT_FAILURE;
    }

    const auto initial = mesh.vertices();
    const EditPreviewLiquifyVertex initial_center = initial[vertex_index(64U, 64U)];
    const EditPreviewLiquifyVertex initial_corner = initial[vertex_index(0U, 0U)];
    if (!require(
            mesh.appendNormalizedPoint(QPointF{0.35, 0.5}, 1.0, 0.18, 0.8, 0.5)
                && mesh.appendNormalizedPoint(QPointF{0.65, 0.5}, 1.0, 0.18, 0.8, 0.5),
            "one normalized push gesture is accepted incrementally"
        )) {
        return EXIT_FAILURE;
    }
    const auto pushed = mesh.vertices();
    const EditPreviewLiquifyVertex pushed_center = pushed[vertex_index(64U, 64U)];
    const EditPreviewLiquifyVertex pushed_corner = pushed[vertex_index(0U, 0U)];
    if (!require(mesh.deformed(), "two distinct points deform the display mesh")
        || !require(
            pushed_center.x > initial_center.x && pushed_center.y == initial_center.y,
            "a horizontal push moves the central mesh position in gesture direction"
        )
        || !require(
            pushed_center.texture_x == initial_center.texture_x
                && pushed_center.texture_y == initial_center.texture_y,
            "the settled preview texture coordinates remain immutable"
        )
        || !require(
            pushed_corner == initial_corner,
            "the pinned outer ring keeps the complete preview rectangle covered"
        )) {
        return EXIT_FAILURE;
    }

    EditPreviewLiquifyMesh repeated;
    repeated.reset(QRectF{10.0, 20.0, 512.0, 384.0}, QRectF{0.0, 0.0, 1.0, 1.0});
    static_cast<void>(
        repeated.appendNormalizedPoint(QPointF{0.35, 0.5}, 1.0, 0.18, 0.8, 0.5)
    );
    static_cast<void>(
        repeated.appendNormalizedPoint(QPointF{0.65, 0.5}, 1.0, 0.18, 0.8, 0.5)
    );
    const auto repeated_vertices = repeated.vertices();
    const auto original_vertices = mesh.vertices();
    if (!require(
            repeated_vertices.size() == original_vertices.size()
                && std::equal(
                    repeated_vertices.begin(),
                    repeated_vertices.end(),
                    original_vertices.begin()
                ),
            "identical incremental input produces deterministic mesh vertices"
        )) {
        return EXIT_FAILURE;
    }

    EditPreviewLiquifyMesh low_pressure;
    low_pressure.reset(QRectF{10.0, 20.0, 512.0, 384.0}, QRectF{0.0, 0.0, 1.0, 1.0});
    static_cast<void>(
        low_pressure.appendNormalizedPoint(QPointF{0.35, 0.5}, 0.25, 0.18, 0.8, 0.5)
    );
    static_cast<void>(
        low_pressure.appendNormalizedPoint(QPointF{0.65, 0.5}, 0.25, 0.18, 0.8, 0.5)
    );
    const double full_pressure_displacement =
        static_cast<double>(pushed_center.x - initial_center.x);
    const double low_pressure_displacement =
        static_cast<double>(
            low_pressure.vertices()[vertex_index(64U, 64U)].x - initial_center.x
        );
    if (!require(
            low_pressure_displacement > 0.0
                && low_pressure_displacement < full_pressure_displacement,
            "pointer pressure scales the transient mesh before the authoritative replay"
        )) {
        return EXIT_FAILURE;
    }

    EditPreviewLiquifyMesh invalid;
    invalid.reset({}, QRectF{0.0, 0.0, 1.0, 1.0});
    if (!require(!invalid.valid(), "empty geometry fails closed")
        || !require(
            !mesh.appendNormalizedPoint(
                QPointF{std::numeric_limits<double>::quiet_NaN(), 0.5},
                1.0,
                0.18,
                0.8,
                0.5
            ),
            "non-finite pointer input is rejected"
        )
        || !require(
            !mesh.appendNormalizedPoint(QPointF{0.5, 0.5}, 1.0, 0.0, 0.8, 0.5),
            "zero-radius brushes are rejected"
        )
        || !require(
            !mesh.appendNormalizedPoint(QPointF{0.5, 0.5}, 1.1, 0.18, 0.8, 0.5),
            "out-of-range pointer pressure is rejected"
        )) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
