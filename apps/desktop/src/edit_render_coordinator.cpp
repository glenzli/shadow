#include "edit_controller.hpp"

#include <QtConcurrent>

#include <QImage>
#include <QSize>
#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <exception>
#include <stdexcept>
#include <utility>

namespace {

// Interactive editing renders one stable prepared source at two JPEG quality
// levels. Keeping the complete render state machine in this translation unit
// makes preview/detail scheduling independently maintainable from Qt controls
// and catalog persistence.
constexpr std::uint32_t EDIT_PREVIEW_EDGE = 2'048;
constexpr std::uint8_t EDIT_PREVIEW_QUALITY = 90;
constexpr std::uint32_t EDIT_INTERACTIVE_PREVIEW_EDGE = 1'536;
constexpr std::uint8_t EDIT_INTERACTIVE_PREVIEW_QUALITY = 84;
constexpr int EDIT_DETAIL_DEBOUNCE_MS = 70;
constexpr int EDIT_DETAIL_WARMUP_IDLE_MS = 180;
constexpr qsizetype EDIT_HISTOGRAM_BIN_COUNT = 256;

[[nodiscard]] bool raw_development_unavailable(const QString& error) noexcept {
    return error.startsWith(QStringLiteral("RAW development is unavailable:"));
}


[[nodiscard]] LocalizedUiMessage
edit_message(const char *const source,
             const std::initializer_list<LocalizedUiArgument> arguments = {}) {
  return {"EditController", source, arguments};
}


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

[[nodiscard]] QVariantMap empty_histogram() {
    return {
        {QStringLiteral("valid"), false},
        {QStringLiteral("updating"), false},
        {QStringLiteral("stale"), false},
        {QStringLiteral("generation"), QVariant::fromValue<qulonglong>(0)},
        {QStringLiteral("targetGeneration"), QVariant::fromValue<qulonglong>(0)},
    };
}

[[nodiscard]] QVariantMap optics_receipt_map(const BackendOpticsReceipt& receipt) {
    return {
        {QStringLiteral("valid"), !receipt.status.isEmpty()},
        {QStringLiteral("status"), receipt.status},
        {QStringLiteral("providerId"), receipt.provider_id},
        {QStringLiteral("providerVersion"), receipt.provider_version},
        {QStringLiteral("cameraProfile"), receipt.camera_profile},
        {QStringLiteral("lensProfile"), receipt.lens_profile},
        {QStringLiteral("distortionAvailable"), receipt.distortion_available},
        {QStringLiteral("tcaAvailable"), receipt.tca_available},
        {QStringLiteral("vignettingAvailable"), receipt.vignetting_available},
        {QStringLiteral("appliedDistortion"), receipt.applied_distortion},
        {QStringLiteral("appliedTca"), receipt.applied_tca},
        {QStringLiteral("appliedVignetting"), receipt.applied_vignetting},
        {QStringLiteral("vignettingUsedDistanceFallback"),
            receipt.vignetting_used_distance_fallback},
        {QStringLiteral("appliedScaling"), receipt.applied_scaling},
    };
}

[[nodiscard]] QVariantMap histogram_snapshot(
    const BackendEditPreviewAnalysis& analysis,
    const quint64 generation
) {
    if (analysis.width == 0 || analysis.height == 0 || analysis.pixel_count == 0
        || analysis.red.size() != EDIT_HISTOGRAM_BIN_COUNT
        || analysis.green.size() != EDIT_HISTOGRAM_BIN_COUNT
        || analysis.blue.size() != EDIT_HISTOGRAM_BIN_COUNT
        || analysis.luma.size() != EDIT_HISTOGRAM_BIN_COUNT
        || analysis.below_zero_samples.size() != 3
        || analysis.above_one_samples.size() != 3) {
        throw std::runtime_error("edit preview analysis violated the desktop contract");
    }
    const double pixel_count = static_cast<double>(analysis.pixel_count);
    return {
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
        {QStringLiteral("approximate"), true},
        {QStringLiteral("scope"), QStringLiteral("complete-warm-proxy")},
        {QStringLiteral("clippingRule"), QStringLiteral("strict-pre-clamp")},
    };
}

} // namespace

void EditController::requestBeforePreview() {
    if (!active_ || !before_preview_source_.isEmpty()) {
        return;
    }
    if (!before_error_message_.isEmpty()) {
    before_error_message_.clear();
        emit beforeErrorTextChanged();
    }
    before_requested_ = true;
    markHistogramUpdating(EditPreviewKind::NeutralBefore);
    maybeStartBeforePreview();
}

void EditController::requestDetailViewport(
    const double center_x,
    const double center_y,
    const int viewport_width_pixels,
    const int viewport_height_pixels
) {
    if (!active_ || !std::isfinite(center_x) || !std::isfinite(center_y)
        || center_x < 0.0 || center_x > 1.0 || center_y < 0.0 || center_y > 1.0
        || viewport_width_pixels <= 0 || viewport_height_pixels <= 0
        || viewport_width_pixels > 8'192 || viewport_height_pixels > 8'192) {
        return;
    }
    detail_center_x_ = center_x;
    detail_center_y_ = center_y;
    detail_viewport_width_ = static_cast<std::uint32_t>(viewport_width_pixels);
    detail_viewport_height_ = static_cast<std::uint32_t>(viewport_height_pixels);
    ++detail_viewport_revision_;
    if (!detail_mode_) {
        detail_mode_ = true;
        emit detailModeChanged();
    }
    if (!detail_error_message_.isEmpty()) {
    detail_error_message_.clear();
        emit detailErrorTextChanged();
    }
    invalidateDetailPresentation(false);
    detail_queued_ = true;
    detail_debounce_.start(EDIT_DETAIL_DEBOUNCE_MS);
}

void EditController::leaveDetailMode() {
    if (!detail_mode_ && detail_tiles_.isEmpty()) {
        return;
    }
    detail_debounce_.stop();
    detail_queued_ = false;
    ++detail_viewport_revision_;
    invalidateDetailPresentation();
    if (detail_mode_) {
        detail_mode_ = false;
        emit detailModeChanged();
    }
    if (!detail_error_message_.isEmpty()) {
        detail_error_message_.clear();
        emit detailErrorTextChanged();
    }
    if (full_resolution_preparing_) {
        setFullResolutionState(false, false, 0);
    }
}


void EditController::finishPreviewTask() {
    EditPreviewTaskResult result = preview_watcher_.result();
    setPreviewRunning(result.generation.kind, false);
    if (close_after_autosave_) {
        preview_queued_ = false;
        before_requested_ = false;
        detail_queued_ = false;
        maybeFinishDeferredApplicationClose();
        return;
    }
    const bool accepted = active_ && accepts_edit_preview(
        result.generation,
        photo_generation_,
        render_revision_
    );
    const bool presentable_current = active_ && can_present_edit_preview(
        result.generation,
        photo_generation_,
        render_revision_
    );

    if (result.generation.kind == EditPreviewKind::Current && presentable_current) {
        if (accepted) {
            settled_render_revision_ = result.generation.current_revision;
        }
        if (!result.error.isEmpty()) {
            if (accepted) {
                markHistogramFailed(EditPreviewKind::Current);
                if (raw_development_unavailable(result.error)) {
                    setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
                        "EditController",
                        "This RAW can be browsed from its embedded preview, but the active local decoder cannot parse it for Precision. Use a compatible local RAW provider or convert it to DNG."
                    )));
                } else {
                    setStatusMessage(edit_message(
                        QT_TRANSLATE_NOOP(
                            "EditController",
                            "Preview render failed · %1"
                        ),
                        {result.error}
                    ));
                }
            }
        } else {
            const QSize dimensions(
                static_cast<int>(result.preview.width),
                static_cast<int>(result.preview.height)
            );
            preview_store_->publish(
                EditPreviewSlot::Current,
                std::move(result.preview.bytes),
                dimensions,
                std::move(result.preview.display_zebra),
                result.generation.current_revision
            );
            preview_source_ = QStringLiteral("image://shadow-edit/current?generation=%1")
                                  .arg(result.generation.current_revision);
            emit previewSourceChanged();
            if (!provisional_preview_source_.isEmpty()) {
                // Publish the authoritative RAW render first, so QML never reveals an empty
                // canvas between the cached Library visual and the local edit preview.
                provisional_preview_source_.clear();
                emit provisionalPreviewSourceChanged();
            }
            if (accepted) {
                const QVariantMap new_receipt = optics_receipt_map(result.preview.optics);
                if (optics_receipt_ != new_receipt) {
                    optics_receipt_ = new_receipt;
                    emit opticsReceiptChanged();
                }
                publishHistogram(
                    EditPreviewKind::Current,
                    result.preview.analysis,
                    result.generation.current_revision
                );
                if (!autosaveFailed()) {
                    setStatusMessage(edit_message(
                        dirty_ ? QT_TRANSLATE_NOOP(
                                     "EditController",
                                     "Saving adjustments · preview is current"
                                 )
                               : QT_TRANSLATE_NOOP(
                                     "EditController",
                                     "Working state and preview are current"
                                 )
                    ));
                }
            }
        }
    } else if (result.generation.kind == EditPreviewKind::NeutralBefore && accepted) {
        before_requested_ = false;
        if (!result.error.isEmpty()) {
            markHistogramFailed(EditPreviewKind::NeutralBefore);
      before_error_message_ = edit_message(
          QT_TRANSLATE_NOOP("EditController", "Neutral baseline failed · %1"),
          {result.error});
            emit beforeErrorTextChanged();
        } else {
            const QSize dimensions(
                static_cast<int>(result.preview.width),
                static_cast<int>(result.preview.height)
            );
            preview_store_->publish(
                EditPreviewSlot::Before,
                std::move(result.preview.bytes),
                dimensions,
                std::move(result.preview.display_zebra),
                result.generation.photo
            );
            publishHistogram(
                EditPreviewKind::NeutralBefore,
                result.preview.analysis,
                result.generation.photo
            );
            before_preview_source_ = QStringLiteral(
                "image://shadow-edit/before?generation=%1"
            ).arg(result.generation.photo);
            emit beforePreviewSourceChanged();
        }
    }

    if (preview_queued_) {
        preview_queued_ = false;
        // The edit that queued this render already advanced render_revision_.
        // Start the latest snapshot without inventing another generation.
        preview_debounce_.start(0);
    } else {
        maybeStartBeforePreview();
        maybeStartDetailRender();
        if (accepted && result.generation.kind == EditPreviewKind::Current
            && result.error.isEmpty()) {
            scheduleDetailWarmup();
        }
    }
    maybeFinishDeferredApplicationClose();
}

void EditController::finishDetailTask() {
    EditDetailTaskResult result = detail_watcher_.result();
    setDetailRunning(false);
    if (close_after_autosave_) {
        detail_queued_ = false;
        before_requested_ = false;
        maybeFinishDeferredApplicationClose();
        return;
    }
    const bool accepted = detail_mode_ && active_ && accepts_edit_detail(
        result.generation,
        photo_generation_,
        render_revision_,
        detail_viewport_revision_
    );
    if (accepted) {
        if (!result.error.isEmpty()) {
            detail_error_message_ = edit_message(
                QT_TRANSLATE_NOOP("EditController", "Full detail failed · %1"),
                {result.error}
            );
            emit detailErrorTextChanged();
            if (full_resolution_preparing_) {
                setFullResolutionState(false, false, 0);
            }
        } else {
            const auto* tile = result.viewport.tiles.size() == 1
                ? &result.viewport.tiles.front()
                : nullptr;
            const std::uint64_t expected_stride = tile == nullptr
                ? 0U
                : static_cast<std::uint64_t>(tile->width) * 3U;
            const std::uint64_t expected_bytes = tile == nullptr
                ? 0U
                : expected_stride * tile->height;
            const bool valid = result.viewport.full_width > 0
                && result.viewport.full_height > 0 && tile != nullptr
                && tile->width > 0 && tile->height > 0
                && tile->row_stride_bytes == expected_stride
                && expected_bytes == static_cast<std::uint64_t>(tile->bytes.size());
            QVector<EditPreviewStore::DetailPublication> publications;
            QVariantList presentation;
            if (valid) {
                auto& mutable_tile = result.viewport.tiles.front();
                const QString ticket = QStringLiteral("viewport-%1-%2")
                                           .arg(mutable_tile.x)
                                           .arg(mutable_tile.y);
                publications.push_back({
                    .ticket = ticket,
                    .bytes = std::move(mutable_tile.bytes),
                    .dimensions = QSize(
                        static_cast<int>(mutable_tile.width),
                        static_cast<int>(mutable_tile.height)
                    ),
                    .row_stride_bytes = static_cast<qsizetype>(
                        mutable_tile.row_stride_bytes
                    ),
                });
                QVariantMap item;
                item.insert(QStringLiteral("x"), mutable_tile.x);
                item.insert(QStringLiteral("y"), mutable_tile.y);
                item.insert(QStringLiteral("width"), mutable_tile.width);
                item.insert(QStringLiteral("height"), mutable_tile.height);
                item.insert(
                    QStringLiteral("source"),
                    QStringLiteral(
                        "image://shadow-edit/detail/%1?photo=%2&recipe=%3&viewport=%4"
                    )
                        .arg(ticket)
                        .arg(result.generation.photo)
                        .arg(result.generation.recipe_revision)
                        .arg(result.generation.viewport_revision)
                );
                presentation.push_back(item);
            }
            if (!valid) {
                detail_error_message_ = edit_message(QT_TRANSLATE_NOOP(
                    "EditController",
                    "Full detail returned an invalid RGB8 tile layout"
                ));
                emit detailErrorTextChanged();
                if (full_resolution_preparing_) {
                    setFullResolutionState(false, false, 0);
                }
            } else {
                const bool geometry_changed = detail_full_width_
                        != result.viewport.full_width
                    || detail_full_height_ != result.viewport.full_height
                    || detail_retained_bytes_ != result.viewport.retained_bytes;
                detail_full_width_ = result.viewport.full_width;
                detail_full_height_ = result.viewport.full_height;
                detail_retained_bytes_ = result.viewport.retained_bytes;
                preview_store_->publishDetails(
                    std::move(publications),
                    result.generation
                );
                detail_tiles_ = std::move(presentation);
                if (geometry_changed) {
                    emit detailGeometryChanged();
                }
                setFullResolutionState(
                    false,
                    true,
                    result.viewport.retained_bytes
                );
                emit detailTilesChanged();
                const double retained_mib = static_cast<double>(detail_retained_bytes_)
                    / (1'024.0 * 1'024.0);
                setStatusMessage(edit_message(
                    QT_TRANSLATE_NOOP(
                        "EditController",
                        "Full-resolution detail ready · %1 MiB local source"
                    ),
                    {LocalizedUiArgument::formattedNumber(retained_mib, 'f', 0)}
                ));
            }
        }
    }

    if (detail_queued_) {
        maybeStartDetailRender();
    } else {
        maybeStartBeforePreview();
    }
    maybeFinishDeferredApplicationClose();
}

void EditController::startPreviewRender() {
    if (!active_ || state_running_) {
        preview_queued_ = active_;
        return;
    }
    if (current_rendering_ || before_rendering_) {
        preview_queued_ = true;
        return;
    }
    setPreviewRunning(EditPreviewKind::Current, true);
    preview_queued_ = false;
    const bool interactive = !active_parameter_gestures_.isEmpty();
    const std::uint32_t max_edge = interactive
        ? EDIT_INTERACTIVE_PREVIEW_EDGE : EDIT_PREVIEW_EDGE;
    const std::uint8_t jpeg_quality = interactive
        ? EDIT_INTERACTIVE_PREVIEW_QUALITY : EDIT_PREVIEW_QUALITY;
  setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
      "EditController", "Rendering preview…")));
    preview_watcher_.setFuture(QtConcurrent::run(
        EditTaskRunner::renderPreview,
        backend_,
        photo_id_,
        source_path_,
        base_commit_id_,
        grade_stack_,
        max_edge,
        jpeg_quality,
        EditPreviewGeneration{
            .kind = EditPreviewKind::Current,
            .photo = photo_generation_,
            .current_revision = render_revision_,
        }
    ));
}

void EditController::startDetailRender() {
    if (!detail_mode_ || !active_) {
        detail_queued_ = false;
        return;
    }
    if (state_running_ || current_rendering_ || before_rendering_
        || settled_render_revision_ != render_revision_ || detail_rendering_) {
        detail_queued_ = true;
        return;
    }
    detail_queued_ = false;
    setDetailRunning(true);
    if (!full_resolution_ready_) {
        setFullResolutionState(true, false, 0);
    }
    setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
        "EditController",
        "Preparing exact full-resolution detail…"
    )));
    detail_watcher_.setFuture(QtConcurrent::run(
        EditTaskRunner::renderDetail,
        backend_,
        photo_id_,
        source_path_,
        base_commit_id_,
        grade_stack_,
        detail_render_token_,
        detail_center_x_,
        detail_center_y_,
        detail_viewport_width_,
        detail_viewport_height_,
        EditDetailGeneration{
            .photo = photo_generation_,
            .recipe_revision = render_revision_,
            .viewport_revision = detail_viewport_revision_,
        }
    ));
}

void EditController::scheduleDetailWarmup() {
    if (!active_ || detail_mode_ || state_running_ || current_rendering_
        || detail_rendering_ || settled_render_revision_ != render_revision_
        || detail_warmup_watcher_.isRunning()) {
        return;
    }
    detail_warmup_debounce_.start(EDIT_DETAIL_WARMUP_IDLE_MS);
}

void EditController::startDetailWarmup() {
    if (!active_ || detail_mode_ || state_running_ || current_rendering_
        || before_rendering_ || detail_rendering_
        || settled_render_revision_ != render_revision_
        || detail_warmup_watcher_.isRunning()) {
        return;
    }
    // This uses the same global cancellation source as foreground detail.
    // Any later pan, zoom, Recipe edit, or photo switch increments it and
    // causes this idle request to be discarded between tiles.
    detail_warmup_token_ = backend_->beginEditDetailRequest();
    if (!full_resolution_ready_) {
        setFullResolutionState(true, false, 0);
    }
    detail_warmup_watcher_.setFuture(QtConcurrent::run(
        EditTaskRunner::warmDetailSource,
        backend_,
        photo_id_,
        source_path_,
        base_commit_id_,
        grade_stack_,
        detail_warmup_token_,
        photo_generation_,
        render_revision_
    ));
}

void EditController::finishDetailWarmupTask() {
    const EditDetailWarmupTaskResult result = detail_warmup_watcher_.result();
    const bool stale = result.photo_generation != photo_generation_
        || result.render_revision != render_revision_;
    const bool superseded = result.error.startsWith(
        QStringLiteral("full detail render was superseded")
    );
    if (stale || superseded) {
        if (full_resolution_preparing_) {
            setFullResolutionState(false, false, 0);
        }
        if (active_ && !detail_mode_
            && settled_render_revision_ == render_revision_) {
            scheduleDetailWarmup();
        }
        return;
    }
    if (result.error.isEmpty()) {
        setFullResolutionState(false, true, result.retained_bytes);
    } else if (full_resolution_preparing_) {
        // A provider error remains visible only if the user explicitly asks
        // for full detail. The status bar should nevertheless stop reporting
        // active background development.
        setFullResolutionState(false, false, 0);
    }
}

void EditController::maybeStartBeforePreview() {
    if (detail_rendering_) {
        return;
    }
    if (!can_start_neutral_before(NeutralBeforeStartState{
            .requested = before_requested_,
            .active = active_,
            .state_task_running = state_running_,
            .current_rendering = current_rendering_,
            .before_rendering = before_rendering_,
            .current_scheduled = preview_debounce_.isActive() || preview_queued_,
            .settled_current_revision = settled_render_revision_,
            .current_revision = render_revision_,
        })) {
        return;
    }
    setPreviewRunning(EditPreviewKind::NeutralBefore, true);
    preview_watcher_.setFuture(QtConcurrent::run(
        EditTaskRunner::renderPreview,
        backend_,
        photo_id_,
        source_path_,
        QString{},
        BackendGradeStack{},
        EDIT_PREVIEW_EDGE,
        EDIT_PREVIEW_QUALITY,
        EditPreviewGeneration{
            .kind = EditPreviewKind::NeutralBefore,
            .photo = photo_generation_,
            .current_revision = 0,
        }
    ));
}

void EditController::maybeStartDetailRender() {
    if (!detail_queued_ || !detail_mode_ || detail_rendering_
        || detail_debounce_.isActive()) {
        return;
    }
    if (state_running_ || current_rendering_ || before_rendering_
        || settled_render_revision_ != render_revision_) {
        return;
    }
    detail_debounce_.start(0);
}

void EditController::invalidateDetailPresentation(const bool discard_tiles) {
    detail_render_token_ = backend_->beginEditDetailRequest();
    if (discard_tiles) {
        preview_store_->clearDetails(EditDetailGeneration{
            .photo = photo_generation_,
            .recipe_revision = render_revision_,
            .viewport_revision = detail_viewport_revision_,
        });
        if (!detail_tiles_.isEmpty()) {
            detail_tiles_.clear();
            emit detailTilesChanged();
        }
    }
}

void EditController::resetDetailState() {
    detail_debounce_.stop();
    detail_warmup_debounce_.stop();
    detail_queued_ = false;
    ++detail_viewport_revision_;
    invalidateDetailPresentation();
    if (detail_mode_) {
        detail_mode_ = false;
        emit detailModeChanged();
    }
    if (!detail_error_message_.isEmpty()) {
        detail_error_message_.clear();
        emit detailErrorTextChanged();
    }
    const bool had_geometry = detail_full_width_ != 0 || detail_full_height_ != 0
        || detail_retained_bytes_ != 0;
    detail_full_width_ = 0;
    detail_full_height_ = 0;
    detail_retained_bytes_ = 0;
    if (had_geometry) {
        emit detailGeometryChanged();
    }
    setFullResolutionState(false, false, 0);
}

void EditController::setFullResolutionState(
    const bool preparing,
    const bool ready,
    const quint64 retained_bytes
) {
    if (full_resolution_preparing_ == preparing
        && full_resolution_ready_ == ready
        && full_resolution_retained_bytes_ == retained_bytes) {
        return;
    }
    full_resolution_preparing_ = preparing;
    full_resolution_ready_ = ready;
    full_resolution_retained_bytes_ = retained_bytes;
    emit fullResolutionStateChanged();
}

void EditController::schedulePreview(const int delay_ms) {
    if (!active_) {
        return;
    }
    // A newly edited Recipe makes an idle full-detail warmup useless. Advance
    // the shared request token before the preview work competes for CPU; the
    // prepared source itself remains reusable, but the old tile render exits
    // at its next cancellation boundary.
    if (detail_warmup_debounce_.isActive() || detail_warmup_watcher_.isRunning()) {
        detail_warmup_debounce_.stop();
        detail_warmup_token_ = backend_->beginEditDetailRequest();
        if (full_resolution_preparing_) {
            setFullResolutionState(false, false, 0);
        }
    }
    ++render_revision_;
    markHistogramUpdating(EditPreviewKind::Current);
    if (detail_mode_) {
        invalidateDetailPresentation();
        detail_queued_ = true;
        detail_debounce_.start(std::max(delay_ms, EDIT_DETAIL_DEBOUNCE_MS));
    }
    if (current_rendering_ || before_rendering_) {
        preview_queued_ = true;
    }
    // A leading-edge throttle keeps the first response prompt during a drag.
    // Repeated slider events do not postpone that first frame indefinitely.
    if (delay_ms <= 0 || !preview_debounce_.isActive()) {
        preview_debounce_.start(delay_ms);
    }
}

void EditController::setPreviewRunning(
    const EditPreviewKind kind,
    const bool running
) {
    const bool previous_busy = busy();
    if (kind == EditPreviewKind::Current) {
        if (current_rendering_ == running) {
            return;
        }
        current_rendering_ = running;
        emit renderingChanged();
    } else {
        if (before_rendering_ == running) {
            return;
        }
        before_rendering_ = running;
        emit beforeRenderingChanged();
    }
    emitBusyChange(previous_busy);
}

void EditController::markHistogramUpdating(const EditPreviewKind kind) {
    QVariantMap& target = kind == EditPreviewKind::Current
        ? histogram_ : before_histogram_;
    target.insert(QStringLiteral("updating"), true);
    target.insert(QStringLiteral("stale"), false);
    target.insert(
        QStringLiteral("targetGeneration"),
        QVariant::fromValue<qulonglong>(
            kind == EditPreviewKind::Current ? render_revision_ : photo_generation_
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
    const quint64 generation
) {
    QVariantMap snapshot = histogram_snapshot(analysis, generation);
    if (kind == EditPreviewKind::Current) {
        histogram_ = std::move(snapshot);
        emit histogramChanged();
    } else {
        before_histogram_ = std::move(snapshot);
        emit beforeHistogramChanged();
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

void EditController::setDetailRunning(const bool running) {
    if (detail_rendering_ == running) {
        return;
    }
    const bool previous_busy = busy();
    detail_rendering_ = running;
    emit detailRenderingChanged();
    emitBusyChange(previous_busy);
}

void EditController::emitBusyChange(const bool previous_busy) {
    if (previous_busy != busy()) {
        emit busyChanged();
    }
}
