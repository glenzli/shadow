#include "preview_diagnostics.hpp"

#include <QColor>
#include <QImage>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <numeric>

namespace {

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
    }
    return condition;
}

[[nodiscard]] std::uint64_t total(const QVector<std::uint32_t>& values) {
    return std::accumulate(
        values.cbegin(),
        values.cend(),
        std::uint64_t{0},
        [](const std::uint64_t sum, const std::uint32_t value) {
            return sum + value;
        }
    );
}

} // namespace

int main() {
    QImage preview(QSize(4, 2), QImage::Format_RGBA8888);
    preview.setPixelColor(0, 0, QColor(128, 128, 128));
    preview.setPixelColor(1, 0, QColor(255, 0, 0));
    preview.setPixelColor(2, 0, QColor(0, 255, 0));
    preview.setPixelColor(3, 0, QColor(0, 0, 255));
    preview.setPixelColor(0, 1, QColor(255, 255, 255));
    preview.setPixelColor(1, 1, QColor(0, 0, 0));
    preview.setPixelColor(2, 1, QColor(210, 160, 120));
    preview.setPixelColor(3, 1, QColor(20, 60, 120));

    const PreviewDisplayScopeAnalysis scope = analyze_display_scope(preview);
    constexpr qsizetype expected_bins = static_cast<qsizetype>(preview_scope_grid_size)
        * preview_scope_grid_size;
    const auto expected_samples = static_cast<std::uint64_t>(preview.width())
        * static_cast<std::uint64_t>(preview.height());

    if (!require(scope.available, "scope analysis is available")
        || !require(scope.source_dimensions == preview.size(), "scope dimensions match the preview")
        || !require(scope.sampled_pixels == expected_samples, "small previews retain every pixel")
        || !require(scope.waveform.size() == expected_bins, "waveform uses the documented grid")
        || !require(scope.parade_red.size() == expected_bins, "red parade uses the documented grid")
        || !require(scope.parade_green.size() == expected_bins, "green parade uses the documented grid")
        || !require(scope.parade_blue.size() == expected_bins, "blue parade uses the documented grid")
        || !require(scope.vectorscope.size() == expected_bins, "vectorscope uses the documented grid")
        || !require(total(scope.waveform) == expected_samples, "waveform counts every sample")
        || !require(total(scope.parade_red) == expected_samples, "red parade counts every sample")
        || !require(total(scope.parade_green) == expected_samples, "green parade counts every sample")
        || !require(total(scope.parade_blue) == expected_samples, "blue parade counts every sample")
        || !require(total(scope.vectorscope) == expected_samples, "vectorscope counts every sample")) {
        return EXIT_FAILURE;
    }

    const int center = preview_scope_grid_size / 2;
    const qsizetype center_index = static_cast<qsizetype>(center) * preview_scope_grid_size + center;
    if (!require(scope.vectorscope.at(center_index) >= 1U,
                 "a neutral pixel is represented at the vectorscope center")) {
        return EXIT_FAILURE;
    }

    QImage qualifier_preview(QSize(3, 1), QImage::Format_RGBA8888);
    qualifier_preview.setPixelColor(0, 0, QColor(255, 0, 0));
    qualifier_preview.setPixelColor(1, 0, QColor(0, 0, 255));
    qualifier_preview.setPixelColor(2, 0, QColor(128, 128, 128));
    const PreviewDisplayScopeAnalysis point_color_scope = analyze_display_scope(
        qualifier_preview,
        PreviewScopeHueQualifier{
            .center_degrees = 30.0,
            .width_degrees = 12.0,
            .softness = 0.0,
        }
    );
    if (!require(point_color_scope.available, "qualified scope remains available")
        || !require(point_color_scope.point_color_qualified,
                    "qualified scope records its Point Color source")
        || !require(point_color_scope.sampled_pixels == 3U,
                    "qualified scope still reports all physical preview samples")
        || !require(point_color_scope.matched_pixels == 1U,
                    "qualified scope includes only the matching Point Color hue")
        || !require(point_color_scope.has_vectorscope_centroid,
                    "qualified scope exposes the selected color centroid")
        || !require(std::abs(point_color_scope.vectorscope_centroid_cb + 0.1145) < 0.01
                        && std::abs(point_color_scope.vectorscope_centroid_cr - 0.5) < 0.01,
                    "qualified centroid follows the selected red display sample")
        || !require(std::isfinite(point_color_scope.skin_guide_deviation_degrees)
                        && std::abs(point_color_scope.skin_guide_deviation_degrees) <= 180.0,
                    "qualified centroid reports a bounded signed skin-guide deviation")
        || !require(total(point_color_scope.vectorscope) > 0U,
                    "qualified scope keeps the selected vectorscope density")
        || !require(total(point_color_scope.vectorscope) < 3U * 256U,
                    "qualified density excludes the nonmatching hue samples")) {
        return EXIT_FAILURE;
    }

    if (!require(!analyze_display_scope(QByteArrayLiteral("not a jpeg")).available,
                 "malformed encoded previews do not produce scope data")) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
