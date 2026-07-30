#include "edit_liquify_coordinates.hpp"

#include <QPointF>

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

[[nodiscard]] bool near(const double actual, const double expected) {
    return std::abs(actual - expected) <= 1.0e-12;
}

[[nodiscard]] bool require_point(
    const std::optional<QPointF>& actual,
    const QPointF expected,
    const char* const message
) {
    if (!actual.has_value() || !near(actual->x(), expected.x())
        || !near(actual->y(), expected.y())) {
        std::cerr << "Liquify coordinate contract failed: " << message << '\n';
        return false;
    }
    return true;
}

} // namespace

int main() {
    if (!require_point(
            EditLiquifyCoordinates::originalPointForOutput(
                QPointF{0.2, 0.3},
                4.0 / 3.0,
                BackendPhotoGeometry{}
            ),
            QPointF{0.2, 0.3},
            "identity Canvas preserves normalized output coordinates"
        )) {
        return EXIT_FAILURE;
    }

    BackendPhotoGeometry crop{
        .crop_left = 0.1,
        .crop_top = 0.2,
        .crop_right = 0.9,
        .crop_bottom = 0.8,
    };
    if (!require_point(
            EditLiquifyCoordinates::originalPointForOutput(QPointF{0.2, 0.3}, 4.0 / 3.0, crop),
            QPointF{0.26, 0.38},
            "crop output maps back into original-image edge coordinates"
        )) {
        return EXIT_FAILURE;
    }

    BackendPhotoGeometry clockwise;
    clockwise.quarter_turn = 1;
    if (!require_point(
            EditLiquifyCoordinates::originalPointForOutput(QPointF{0.2, 0.3}, 3.0 / 4.0, clockwise),
            QPointF{0.3, 0.8},
            "clockwise output is inverse-rotated before persistence"
        )) {
        return EXIT_FAILURE;
    }

    BackendPhotoGeometry flipped;
    flipped.flip_horizontal = true;
    if (!require_point(
            EditLiquifyCoordinates::originalPointForOutput(QPointF{0.2, 0.3}, 4.0 / 3.0, flipped),
            QPointF{0.8, 0.3},
            "horizontal output flip is inverted before persistence"
        )) {
        return EXIT_FAILURE;
    }

    BackendPhotoGeometry straightened;
    straightened.straighten_degrees = 17.0;
    if (!require_point(
            EditLiquifyCoordinates::originalPointForOutput(
                QPointF{0.5, 0.5},
                4.0 / 3.0,
                straightened
            ),
            QPointF{0.5, 0.5},
            "straighten keeps the shared rotation centre fixed"
        )) {
        return EXIT_FAILURE;
    }

    BackendPhotoGeometry invalid;
    invalid.quarter_turn = 4;
    if (EditLiquifyCoordinates::originalPointForOutput(QPointF{0.5, 0.5}, 1.0, invalid).has_value()
        || EditLiquifyCoordinates::originalPointForOutput(
               QPointF{0.5, 0.5},
               0.0,
               BackendPhotoGeometry{}
        )
               .has_value()) {
        std::cerr
            << "Liquify coordinate contract failed: malformed Canvas input must fail closed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
