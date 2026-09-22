#include "edit_controller.hpp"

#include "edit_performance_diagnostics.hpp"
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
    queueDisplayScope(EditPreviewKind::Current, snapshot.bytes, generation);
}

void EditController::queueDisplayScope(
    const EditPreviewKind kind,
    QByteArray encoded_preview,
    const quint64 generation
) {
    if (encoded_preview.isEmpty()) {
        return;
    }
    const bool current = kind == EditPreviewKind::Current;
    const std::optional<PreviewScopeHueQualifier> qualifier =
        current && point_color_scope_active_ ? selectedPointColorScopeQualifier() : std::nullopt;
    if (current && point_color_scope_active_ && !qualifier.has_value()) {
        return;
    }
    QVariantMap& target = current ? histogram_ : before_histogram_;
    target.insert(QStringLiteral("displayScopeAvailable"), false);
    target.insert(QStringLiteral("displayScopeUpdating"), true);
    if (current) {
        emit histogramChanged();
    } else {
        emit beforeHistogramChanged();
    }
    EditDisplayScopeTaskInput input{
        .kind = kind,
        .photo_generation = photo_generation_,
        .preview_generation = generation,
        .request_revision = current ? ++current_scope_request_revision_
                                    : ++before_scope_request_revision_,
        .reference_epoch = point_color_reference_epoch_,
        .encoded_preview = std::move(encoded_preview),
        .decoded_preview = current && current_scope_image_generation_ == generation
            ? current_scope_image_ : QImage{},
        .qualifier = qualifier,
        .reference = current && qualifier.has_value() ? point_color_scope_reference_
                                                      : std::nullopt,
    };
    if (current) {
        pending_current_scope_ = std::move(input);
    } else {
        pending_before_scope_ = std::move(input);
    }
    startDisplayScopeTask();
}

void EditController::startDisplayScopeTask() {
    if (!work_scheduler_.admitsDisplayScope(workAdmissionState())) {
        return;
    }
    std::optional<EditDisplayScopeTaskInput> input;
    if (pending_current_scope_.has_value()) {
        input = std::move(pending_current_scope_);
        pending_current_scope_.reset();
    } else if (pending_before_scope_.has_value()) {
        input = std::move(pending_before_scope_);
        pending_before_scope_.reset();
    }
    if (!input.has_value()) {
        return;
    }
    if (input->kind == EditPreviewKind::Current && input->qualifier.has_value()
        && input->reference_epoch == point_color_reference_epoch_
        && point_color_scope_reference_.has_value()
        && point_color_scope_reference_->qualifier.center_degrees
            == input->qualifier->center_degrees
        && point_color_scope_reference_->qualifier.width_degrees
            == input->qualifier->width_degrees
        && point_color_scope_reference_->qualifier.softness
            == input->qualifier->softness) {
        input->reference = point_color_scope_reference_;
    }
    display_scope_task_active_ = true;
    display_scope_watcher_.setFuture(work_scheduler_.runDisplayScope(std::move(*input)));
}

void EditController::finishDisplayScopeTask() {
    EditDisplayScopeTaskResult result = display_scope_watcher_.result();
    display_scope_task_active_ = false;
    const bool current = result.kind == EditPreviewKind::Current;
    // Capturing the selector from the first ready preview is independent of
    // accepting its scope overlay. A newer Recipe may already be rendering.
    if (current && result.reference.has_value() && !point_color_scope_reference_.has_value()
        && result.photo_generation == photo_generation_
        && result.reference_epoch == point_color_reference_epoch_
        && point_color_scope_active_) {
        const auto qualifier = selectedPointColorScopeQualifier();
        if (qualifier.has_value()
            && qualifier->center_degrees == result.reference->qualifier.center_degrees
            && qualifier->width_degrees == result.reference->qualifier.width_degrees
            && qualifier->softness == result.reference->qualifier.softness) {
            point_color_scope_reference_ = result.reference;
        }
    }
    QVariantMap& target = current ? histogram_ : before_histogram_;
    const bool valid_generation = target.value(QStringLiteral("generation")).toULongLong()
        == result.preview_generation;
    const bool current_request = result.request_revision
        == (current ? current_scope_request_revision_ : before_scope_request_revision_);
    const bool current_recipe = current ? render_revision_ == result.preview_generation
                                        : before_preview_state_.revision()
                                            == result.preview_generation;
    if (active_ && result.photo_generation == photo_generation_ && valid_generation
        && current_request && current_recipe && target.value(QStringLiteral("valid")).toBool()) {
        if (current) {
            current_scope_image_ = std::move(result.decoded_preview);
            current_scope_image_generation_ = result.preview_generation;
            if (result.reference_epoch == point_color_reference_epoch_) {
                point_color_scope_reference_ = std::move(result.reference);
            }
        }
        const QVariantMap scope_snapshot = display_scope_snapshot(result.scope);
        for (auto iterator = scope_snapshot.cbegin(); iterator != scope_snapshot.cend(); ++iterator) {
            target.insert(iterator.key(), iterator.value());
        }
        target.insert(QStringLiteral("displayScopeUpdating"), false);
        if (current) {
            emit histogramChanged();
        } else {
            emit beforeHistogramChanged();
        }
        log_edit_performance_checkpoint(
            current ? "current-display-scope-published" : "before-display-scope-published",
            result.photo_generation,
            result.preview_generation
        );
    }
    startDisplayScopeTask();
    if (!display_scope_task_active_ && !pending_current_scope_.has_value()
        && !pending_before_scope_.has_value()) {
        scheduleDetailWarmup();
    }
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
    ++before_scope_request_revision_;
    pending_before_scope_.reset();
    current_scope_image_ = {};
    current_scope_image_generation_ = 0;
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
