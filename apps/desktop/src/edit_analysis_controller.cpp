#include "edit_controller.hpp"

#include "preview_diagnostics.hpp"

#include <QVariantMap>

#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

namespace {

constexpr qsizetype EDIT_HISTOGRAM_BIN_COUNT = 256;
constexpr qsizetype EDIT_HDR_HEADROOM_BIN_COUNT = 16;

[[nodiscard]] QVariantList histogram_counts(
    const QVector<std::uint64_t>& counts
) {
    QVariantList result;
    result.reserve(counts.size());
    for (const auto count : counts) {
        result.push_back(QVariant::fromValue<qulonglong>(count));
    }
    return result;
}
[[nodiscard]] QVariantList scope_counts(
    const QVector<std::uint32_t>& counts
) {
    QVariantList result;
    result.reserve(counts.size());
    for (const auto count : counts) {
        result.push_back(static_cast<unsigned int>(count));
    }
    return result;
}

[[nodiscard]] QVariantMap display_scope_snapshot(
    const PreviewDisplayScopeAnalysis& scope
) {
    const qsizetype expected_count = static_cast<qsizetype>(preview_scope_grid_size)
        * preview_scope_grid_size;
    if (!scope.available || scope.source_dimensions.isEmpty()
        || scope.waveform.size() != expected_count
        || scope.parade_red.size() != expected_count
        || scope.parade_green.size() != expected_count
        || scope.parade_blue.size() != expected_count
        || scope.vectorscope.size() != expected_count) {
        return {{QStringLiteral("displayScopeAvailable"), false}};
    }
    QVariantMap result{
        {QStringLiteral("displayScopeAvailable"), true},
        {QStringLiteral("displayScopeGridSize"), preview_scope_grid_size},
        {QStringLiteral("displayScopeWidth"), scope.source_dimensions.width()},
        {QStringLiteral("displayScopeHeight"), scope.source_dimensions.height()},
        {
            QStringLiteral("displayScopeSampledPixels"),
            QVariant::fromValue<qulonglong>(scope.sampled_pixels)
        },
        {
            QStringLiteral("displayScopeMatchedPixels"),
            QVariant::fromValue<qulonglong>(scope.matched_pixels)
        },
        {QStringLiteral("displayScopePointColorQualified"), scope.point_color_qualified},
        {QStringLiteral("displayScopeReferenceSelection"), scope.reference_selection},
        {QStringLiteral("displayScopeCentroidAvailable"), scope.has_vectorscope_centroid},
        {QStringLiteral("displayScopeCentroidCb"), scope.vectorscope_centroid_cb},
        {QStringLiteral("displayScopeCentroidCr"), scope.vectorscope_centroid_cr},
        {
            QStringLiteral("displayScopeSkinGuideDeviationDegrees"),
            scope.skin_guide_deviation_degrees
        },
        {QStringLiteral("displayWaveform"), scope_counts(scope.waveform)},
        {QStringLiteral("displayParadeRed"), scope_counts(scope.parade_red)},
        {QStringLiteral("displayParadeGreen"), scope_counts(scope.parade_green)},
        {QStringLiteral("displayParadeBlue"), scope_counts(scope.parade_blue)},
        {QStringLiteral("displayVectorscope"), scope_counts(scope.vectorscope)},
    };
    constexpr std::array<const char*, preview_skin_tone_range_count> range_names{
        "Shadows",
        "Midtones",
        "Highlights",
    };
    for (std::size_t index = 0U; index < preview_skin_tone_range_count; ++index) {
        const auto& range = scope.skin_tone_ranges[index];
        const QString prefix = QStringLiteral("displayScopeSkin")
            + QString::fromLatin1(range_names[index]);
        result.insert(prefix + QStringLiteral("Available"), range.available);
        result.insert(
            prefix + QStringLiteral("MatchedPixels"),
            QVariant::fromValue<qulonglong>(range.matched_pixels)
        );
        result.insert(prefix + QStringLiteral("CentroidCb"), range.vectorscope_centroid_cb);
        result.insert(prefix + QStringLiteral("CentroidCr"), range.vectorscope_centroid_cr);
        result.insert(
            prefix + QStringLiteral("DeviationDegrees"),
            range.skin_guide_deviation_degrees
        );
    }
    return result;
}

[[nodiscard]] QVariantMap empty_histogram() {
    return {
        {QStringLiteral("valid"), false},
        {QStringLiteral("updating"), false},
        {QStringLiteral("stale"), false},
        {QStringLiteral("generation"), QVariant::fromValue<qulonglong>(0)},
        {QStringLiteral("targetGeneration"), QVariant::fromValue<qulonglong>(0)},
    };
}

[[nodiscard]] QVariantMap histogram_snapshot(
    const BackendEditPreviewAnalysis& analysis,
    const PreviewDisplayScopeAnalysis& display_scope,
    const quint64 generation
) {
    if (analysis.width == 0 || analysis.height == 0 || analysis.pixel_count == 0
        || !analysis.available
        || analysis.red.size() != EDIT_HISTOGRAM_BIN_COUNT
        || analysis.green.size() != EDIT_HISTOGRAM_BIN_COUNT
        || analysis.blue.size() != EDIT_HISTOGRAM_BIN_COUNT
        || analysis.luma.size() != EDIT_HISTOGRAM_BIN_COUNT
        || analysis.below_zero_samples.size() != 3
        || analysis.above_one_samples.size() != 3
        || analysis.hdr_headroom_bins.size() != EDIT_HDR_HEADROOM_BIN_COUNT
        || analysis.hdr_headroom_pixels > analysis.pixel_count
        || !std::isfinite(analysis.hdr_peak_headroom_ev)
        || analysis.hdr_peak_headroom_ev < 0.0
        || (analysis.hdr_headroom_pixels == 0 && analysis.hdr_peak_headroom_ev != 0.0)
        || (analysis.hdr_headroom_pixels > 0 && analysis.hdr_peak_headroom_ev <= 0.0)) {
        throw std::runtime_error("edit preview analysis violated the desktop contract");
    }
    std::uint64_t hdr_headroom_sum = 0;
    for (const auto count : analysis.hdr_headroom_bins) {
        if (count > analysis.pixel_count
            || std::numeric_limits<std::uint64_t>::max() - hdr_headroom_sum < count) {
            throw std::runtime_error("edit preview HDR headroom bins overflow the desktop contract");
        }
        hdr_headroom_sum += count;
    }
    if (hdr_headroom_sum != analysis.hdr_headroom_pixels) {
        throw std::runtime_error("edit preview HDR headroom bins do not cover their pixels");
    }
    const double pixel_count = static_cast<double>(analysis.pixel_count);
    QVariantMap snapshot{
        {QStringLiteral("valid"), true},
        {QStringLiteral("updating"), false},
        {QStringLiteral("stale"), false},
        {QStringLiteral("generation"), QVariant::fromValue<qulonglong>(generation)},
        {QStringLiteral("targetGeneration"), QVariant::fromValue<qulonglong>(generation)},
        {QStringLiteral("version"), analysis.version},
        {QStringLiteral("width"), analysis.width},
        {QStringLiteral("height"), analysis.height},
        {
            QStringLiteral("pixelCount"),
            QVariant::fromValue<qulonglong>(analysis.pixel_count)
        },
        {QStringLiteral("red"), histogram_counts(analysis.red)},
        {QStringLiteral("green"), histogram_counts(analysis.green)},
        {QStringLiteral("blue"), histogram_counts(analysis.blue)},
        {QStringLiteral("luma"), histogram_counts(analysis.luma)},
        {QStringLiteral("belowZero"), histogram_counts(analysis.below_zero_samples)},
        {QStringLiteral("aboveOne"), histogram_counts(analysis.above_one_samples)},
        {QStringLiteral("hdrHeadroomBins"), histogram_counts(analysis.hdr_headroom_bins)},
        {
            QStringLiteral("hdrHeadroomPixels"),
            QVariant::fromValue<qulonglong>(analysis.hdr_headroom_pixels)
        },
        {QStringLiteral("hdrPeakHeadroomEv"), analysis.hdr_peak_headroom_ev},
        {
            QStringLiteral("shadowClippedPixels"),
            QVariant::fromValue<qulonglong>(analysis.shadow_clipped_pixels)
        },
        {
            QStringLiteral("highlightClippedPixels"),
            QVariant::fromValue<qulonglong>(analysis.highlight_clipped_pixels)
        },
        {
            QStringLiteral("shadowClippedFraction"),
            static_cast<double>(analysis.shadow_clipped_pixels) / pixel_count
        },
        {
            QStringLiteral("highlightClippedFraction"),
            static_cast<double>(analysis.highlight_clipped_pixels) / pixel_count
        },
        {
            QStringLiteral("hdrHeadroomFraction"),
            static_cast<double>(analysis.hdr_headroom_pixels) / pixel_count
        },
        {QStringLiteral("approximate"), true},
        {QStringLiteral("scope"), QStringLiteral("complete-warm-proxy")},
        {QStringLiteral("clippingRule"), QStringLiteral("strict-pre-clamp")},
    };
    const QVariantMap scope_snapshot = display_scope_snapshot(display_scope);
    for (auto iterator = scope_snapshot.cbegin(); iterator != scope_snapshot.cend(); ++iterator) {
        snapshot.insert(iterator.key(), iterator.value());
    }
    return snapshot;
}

} // namespace

void EditController::markHistogramUpdating(const EditPreviewKind kind) {
    QVariantMap& target = kind == EditPreviewKind::Current
        ? histogram_ : before_histogram_;
    target.insert(QStringLiteral("updating"), true);
    target.insert(QStringLiteral("stale"), false);
    target.insert(
        QStringLiteral("targetGeneration"),
        QVariant::fromValue<qulonglong>(
            kind == EditPreviewKind::Current ? render_revision_ : before_preview_state_.revision()
        )
    );
    if (kind == EditPreviewKind::Current) {
        emit histogramChanged();
    } else {
        emit beforeHistogramChanged();
    }
}

void EditController::publishHistogram(
    const EditPreviewKind kind,
    const BackendEditPreviewAnalysis& analysis,
    const PreviewDisplayScopeAnalysis& display_scope,
    const quint64 generation
) {
    QVariantMap snapshot = histogram_snapshot(analysis, display_scope, generation);
    if (kind == EditPreviewKind::Current) {
        histogram_ = std::move(snapshot);
        emit histogramChanged();
    } else {
        before_histogram_ = std::move(snapshot);
        emit beforeHistogramChanged();
    }
}

PreviewDisplayScopeAnalysis EditController::analyzeCurrentDisplayScope(
    const QByteArray& encoded_preview,
    const std::optional<PreviewScopeHueQualifier>& point_color_qualifier
) {
    if (!point_color_scope_active_ || !point_color_qualifier.has_value()) {
        clearPointColorScopeReference();
        return analyze_display_scope(encoded_preview, point_color_qualifier);
    }

    const auto same_qualifier = [&point_color_qualifier](
                                    const PreviewScopeReferenceSelection& reference
                                ) {
        return reference.qualifier.center_degrees
                == point_color_qualifier->center_degrees
            && reference.qualifier.width_degrees
                == point_color_qualifier->width_degrees
            && reference.qualifier.softness == point_color_qualifier->softness;
    };
    if (point_color_scope_reference_.has_value()
        && same_qualifier(*point_color_scope_reference_)) {
        const PreviewDisplayScopeAnalysis frozen = analyze_display_scope(
            encoded_preview,
            *point_color_scope_reference_
        );
        if (frozen.available) {
            return frozen;
        }
    }

    point_color_scope_reference_ = capture_display_scope_reference(
        encoded_preview,
        *point_color_qualifier
    );
    if (point_color_scope_reference_->available) {
        return analyze_display_scope(encoded_preview, *point_color_scope_reference_);
    }
    clearPointColorScopeReference();
    return analyze_display_scope(encoded_preview, point_color_qualifier);
}

void EditController::refreshCurrentDisplayScope() {
    if (!histogram_.value(QStringLiteral("valid")).toBool()) {
        return;
    }
    bool valid_generation = false;
    const quint64 generation = histogram_.value(QStringLiteral("generation")).toULongLong(
        &valid_generation
    );
    if (!valid_generation) {
        return;
    }
    const auto snapshot = preview_store_->snapshot(EditPreviewSlot::Current, generation);
    if (snapshot.bytes.isEmpty()) {
        return;
    }
    const std::optional<PreviewScopeHueQualifier> point_color_qualifier =
        point_color_scope_active_ ? selectedPointColorScopeQualifier() : std::nullopt;
    if (point_color_scope_active_ && !point_color_qualifier.has_value()) {
        return;
    }
    const PreviewDisplayScopeAnalysis display_scope = analyzeCurrentDisplayScope(
        snapshot.bytes,
        point_color_qualifier
    );
    const QVariantMap scope_snapshot = display_scope_snapshot(display_scope);
    for (auto iterator = scope_snapshot.cbegin(); iterator != scope_snapshot.cend(); ++iterator) {
        histogram_.insert(iterator.key(), iterator.value());
    }
    emit histogramChanged();
}

void EditController::markHistogramFailed(const EditPreviewKind kind) {
    QVariantMap& target = kind == EditPreviewKind::Current
        ? histogram_ : before_histogram_;
    target.insert(QStringLiteral("updating"), false);
    target.insert(QStringLiteral("stale"), true);
    if (kind == EditPreviewKind::Current) {
        emit histogramChanged();
    } else {
        emit beforeHistogramChanged();
    }
}

void EditController::clearHistograms() {
    clearPointColorScopeReference();
    const QVariantMap empty = empty_histogram();
    if (histogram_ != empty) {
        histogram_ = empty;
        emit histogramChanged();
    }
    if (before_histogram_ != empty) {
        before_histogram_ = empty;
        emit beforeHistogramChanged();
    }
}
