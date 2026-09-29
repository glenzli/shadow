#include "edit_controller.hpp"
#include "edit_auto_geometry_controller.hpp"
#include "edit_preview_presentation_context.hpp"
#include "preview_diagnostics.hpp"

#include <QtConcurrent>

#include <QImage>
#include <QDebug>
#include <QSize>
#include <QVariantMap>

#include <exception>
#include <utility>

namespace {

// Interactive and settled overview frames deliberately share one prepared
// 1536px source. Using a second edge for slider gestures caused the first edit
// to decode and develop the RAW again instead of reusing the proxy prepared
// when the photo opened. Full-resolution inspection remains a separate,
// explicitly warmed detail path.
constexpr std::uint32_t EDIT_PREVIEW_EDGE = 1'536;
constexpr std::uint8_t EDIT_PREVIEW_QUALITY = 90;
constexpr std::uint8_t EDIT_INTERACTIVE_PREVIEW_QUALITY = 84;

[[nodiscard]] bool raw_development_unavailable(const QString& error) noexcept {
    return error.startsWith(QStringLiteral("RAW development is unavailable:"));
}

[[nodiscard]] bool interactive_timing_enabled() {
    return qEnvironmentVariable("SHADOW_INTERACTIVE_TIMING") == QStringLiteral("1");
}


[[nodiscard]] LocalizedUiMessage
edit_message(const char *const source,
             const std::initializer_list<LocalizedUiArgument> arguments = {}) {
    return {"EditController", source, arguments};
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


} // namespace

void EditController::requestBeforePreview() {
    if (!active_) {
        return;
    }
    refreshBeforePreviewContext();
    if (!before_preview_source_.isEmpty()) {
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


void EditController::finishPreviewTask() {
    EditPreviewTaskResult result = preview_watcher_.result();
    const EditPreviewKind kind = result.generation.kind();
    if (interactive_preview_timing_token_ == result.generation.render_token) {
        qInfo().noquote()
            << QStringLiteral("shadow.interactive-timing token=%1 component=desktop stage=task-finished elapsed_ms=%2 terminal=%3 accepted-candidate=%4")
                   .arg(result.generation.render_token)
                   .arg(interactive_preview_timing_.elapsed())
                   .arg(result.terminal == EditPreviewTerminal::Completed
                            ? QStringLiteral("completed")
                            : QStringLiteral("cancelled"))
                   .arg(active_ && accepts_edit_preview(
                       result.generation,
                       photo_generation_,
                       render_revision_,
                       before_preview_state_.revision()
                   ));
        interactive_preview_timing_token_ = 0;
    }
    if (preview_render_token_ == result.generation.render_token) {
        preview_render_token_ = 0;
    }
    setPreviewRunning(kind, false);
    if (persistence_state_.closeAfterAutosave()) {
        preview_queued_ = false;
        before_requested_ = false;
        detail_queued_ = false;
        maybeFinishDeferredApplicationClose();
        return;
    }
    if (result.terminal == EditPreviewTerminal::Cancelled) {
        // Cancellation is a successful control-flow outcome. It must not
        // publish pixels, histogram, optics receipts, settled revisions,
        // errors, or trigger detail warmup. The latest queued snapshot starts
        // as soon as this first-phase host cancellation reaches the worker
        // boundary; native checkpoint cancellation is added separately.
        if (preview_queued_) {
            preview_queued_ = false;
            preview_debounce_.start(0);
        } else {
            maybeStartBeforePreview();
            maybeStartDetailRender();
        }
        maybeFinishDeferredApplicationClose();
        return;
    }
    const bool accepted = active_ && accepts_edit_preview(
        result.generation,
        photo_generation_,
        render_revision_,
        before_preview_state_.revision()
    );
    const bool presentable_current = active_ && can_present_edit_preview(
        result.generation,
        photo_generation_,
        render_revision_
    );

    if (kind == EditPreviewKind::Current && presentable_current) {
        if (!result.error.isEmpty()) {
            if (accepted) {
                if (auto_geometry_controller_) {
                    auto_geometry_controller_->handlePreviewFailed(
                        result.generation.current_revision,
                        result.error
                    );
                }
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
            const std::optional<PreviewScopeHueQualifier> point_color_qualifier =
                point_color_scope_active_ ? selectedPointColorScopeQualifier() : std::nullopt;
            const PreviewDisplayScopeAnalysis display_scope = accepted
                    && result.preview.analysis.available
                ? analyzeCurrentDisplayScope(result.preview.bytes, point_color_qualifier)
                : PreviewDisplayScopeAnalysis{};
            if (result.generation.policy == EditPreviewPolicy::Interactive
                && !active_parameter_gestures_.isEmpty()) {
                first_interactive_frame_presented_ = true;
            }
            if (accepted
                && (result.generation.policy == EditPreviewPolicy::Settled
                    || result.generation.policy == EditPreviewPolicy::PresentationCommit)
                && result.preview.analysis.available) {
                settled_render_revision_ = result.generation.current_revision;
            }
            const QSize dimensions(
                static_cast<int>(result.preview.width),
                static_cast<int>(result.preview.height)
            );
            if (level_zero_width_ != result.preview.level_zero_width
                || level_zero_height_ != result.preview.level_zero_height) {
                level_zero_width_ = result.preview.level_zero_width;
                level_zero_height_ = result.preview.level_zero_height;
                emit previewGeometryChanged();
            }
            preview_store_->publish(
                EditPreviewSlot::Current,
                std::move(result.preview.bytes),
                dimensions,
                static_cast<qsizetype>(result.preview.row_stride_bytes),
                std::move(result.preview.display_zebra),
                result.generation.current_revision,
                std::move(result.preview.frame),
                result.generation.presentation_binding
            );
            if (accepted && result.generation.policy == EditPreviewPolicy::Settled
                && auto_geometry_controller_) {
                auto_geometry_controller_->handlePreviewSettled(
                    result.generation.current_revision
                );
            }
            preview_source_ = QStringLiteral("image://shadow-edit/current?generation=%1")
                                  .arg(result.generation.current_revision);
            emit previewSourceChanged();
            if (accepted) {
                publishMaskCoverage(
                    std::move(result.preview.mask_coverage),
                    result.generation
                );
            }
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
                if (result.preview.analysis.available) {
                    publishHistogram(
                        EditPreviewKind::Current,
                        result.preview.analysis,
                        display_scope,
                        result.generation.current_revision
                    );
                }
                if (!autosaveFailed()) {
                    if (result.generation.policy == EditPreviewPolicy::Settled) {
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
        }
    } else if (kind == EditPreviewKind::NeutralBefore && accepted) {
        before_requested_ = false;
        if (!result.error.isEmpty()) {
            markHistogramFailed(EditPreviewKind::NeutralBefore);
      before_error_message_ = edit_message(
          QT_TRANSLATE_NOOP("EditController", "Neutral baseline failed · %1"),
          {result.error});
            emit beforeErrorTextChanged();
        } else {
            const PreviewDisplayScopeAnalysis display_scope = result.preview.analysis.available
                ? analyze_display_scope(result.preview.bytes)
                : PreviewDisplayScopeAnalysis{};
            const QSize dimensions(
                static_cast<int>(result.preview.width),
                static_cast<int>(result.preview.height)
            );
            preview_store_->publish(
                EditPreviewSlot::Before,
                std::move(result.preview.bytes),
                dimensions,
                static_cast<qsizetype>(result.preview.row_stride_bytes),
                std::move(result.preview.display_zebra),
                result.generation.before_revision
            );
            publishHistogram(
                EditPreviewKind::NeutralBefore,
                result.preview.analysis,
                display_scope,
                result.generation.before_revision
            );
            before_preview_source_ = QStringLiteral(
                "image://shadow-edit/before?generation=%1"
            ).arg(result.generation.before_revision);
            emit beforePreviewSourceChanged();
        }
    }

    if (result.generation.policy == EditPreviewPolicy::PresentationCommit) {
        if (accepted && result.terminal == EditPreviewTerminal::Completed
            && result.error.isEmpty()) {
            // The bridge returns only after the exact Recipe preview blob and
            // source-checked Catalog record are durable. activeChanged now
            // becomes the safe Library refresh notification.
            finalizePhotoClose();
            return;
        }
        if (result.terminal != EditPreviewTerminal::Cancelled) {
            // A cache/storage failure must not trap the user on a hidden
            // Precision page. Preserve the saved Recipe, close cleanly, and
            // let the next Library request fall back to the last valid visual.
            finalizePhotoClose();
            return;
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
        if (accepted && result.generation.policy == EditPreviewPolicy::Settled
            && result.error.isEmpty()) {
            scheduleDetailWarmup();
        }
    }
    maybeFinishDeferredApplicationClose();
}

void EditController::startPreviewRender() {
    if (!active_ || stateTaskRunning()) {
        preview_queued_ = active_;
        return;
    }
    if (current_rendering_ || before_rendering_) {
        preview_queued_ = true;
        return;
    }
    setPreviewRunning(EditPreviewKind::Current, true);
    preview_queued_ = false;
    const bool interactive = !presentation_commit_requested_
                             && !active_parameter_gestures_.isEmpty();
    const EditPreviewPolicy policy = presentation_commit_requested_
        ? EditPreviewPolicy::PresentationCommit
        : (interactive ? EditPreviewPolicy::Interactive : EditPreviewPolicy::Settled);
    const std::uint32_t max_edge = EDIT_PREVIEW_EDGE;
    const std::uint8_t jpeg_quality = interactive
        ? EDIT_INTERACTIVE_PREVIEW_QUALITY : EDIT_PREVIEW_QUALITY;
    preview_render_token_ = backend_->beginEditPreviewRequest();
    if (interactive && interactive_timing_enabled()) {
        interactive_preview_timing_.restart();
        interactive_preview_timing_token_ = preview_render_token_;
        qInfo().noquote()
            << QStringLiteral("shadow.interactive-timing token=%1 component=desktop stage=task-dispatched")
                   .arg(preview_render_token_);
    } else {
        interactive_preview_timing_token_ = 0;
    }
    in_flight_preview_policy_ = policy;
    BackendGradeStack preview_stack = grade_stack_;
    if (auto_geometry_controller_) {
        (void)auto_geometry_controller_->applyPreviewOverride(preview_stack.geometry);
    }
    if (crop_tool_active_) {
        preview_stack.geometry.crop_left = 0.0;
        preview_stack.geometry.crop_top = 0.0;
        preview_stack.geometry.crop_right = 1.0;
        preview_stack.geometry.crop_bottom = 1.0;
    }
    EditPreviewGeneration generation{
        .policy = policy,
        .photo = photo_generation_,
        .current_revision = render_revision_,
        .render_token = preview_render_token_,
        .recipe_revision = working_revision_,
        .mask_coverage_request = currentMaskCoverageRequest(preview_stack),
        .presentation_binding = interactive && preview_presentation_context_
                                    ? preview_presentation_context_->snapshot()
                                    : EditPreviewPresentationBinding{},
    };
    if (generation.mask_coverage_request.has_value()) {
        mask_coverage_refresh_pending_ = false;
        preview_store_->expectMaskCoverage(
            maskCoverageGeneration(generation)
        );
    }
    setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
        "EditController", "Rendering preview…")));
    preview_watcher_.setFuture(QtConcurrent::run(
        EditTaskRunner::renderPreview,
        backend_,
        photo_id_,
        source_path_,
        base_commit_id_,
        std::move(preview_stack),
        preview_render_token_,
        max_edge,
        jpeg_quality,
        std::move(generation)
    ));
}

void EditController::maybeStartBeforePreview() {
    refreshBeforePreviewContext();
    if (detail_rendering_) {
        return;
    }
    if (!can_start_neutral_before(NeutralBeforeStartState{
            .requested = before_requested_,
            .active = active_,
            .state_task_running = stateTaskRunning(),
            .current_rendering = current_rendering_,
            .before_rendering = before_rendering_,
            .current_scheduled = preview_debounce_.isActive() || preview_queued_,
            .settled_current_revision = settled_render_revision_,
            .current_revision = render_revision_,
        })) {
        return;
    }
    setPreviewRunning(EditPreviewKind::NeutralBefore, true);
    preview_render_token_ = backend_->beginEditPreviewRequest();
    in_flight_preview_policy_ = EditPreviewPolicy::NeutralBefore;
    preview_watcher_.setFuture(QtConcurrent::run(
        EditTaskRunner::renderPreview,
        backend_,
        photo_id_,
        source_path_,
        QString{},
        before_preview_state_.stack(),
        preview_render_token_,
        EDIT_PREVIEW_EDGE,
        EDIT_PREVIEW_QUALITY,
        EditPreviewGeneration{
            .policy = EditPreviewPolicy::NeutralBefore,
            .photo = photo_generation_,
            .current_revision = 0,
            .render_token = preview_render_token_,
            .recipe_revision = 0,
            .before_revision = before_preview_state_.revision(),
        }
    ));
}

void EditController::refreshBeforePreviewContext() {
    if (!active_)
        return;
    auto geometry = grade_stack_.geometry;
    if (auto_geometry_controller_)
        (void)auto_geometry_controller_->applyPreviewOverride(geometry);
    if (!before_preview_state_.observe(
            photo_generation_, neutral_before_stack(grade_stack_, geometry, crop_tool_active_)))
        return;
    const bool requested = before_requested_ || before_rendering_
                           || !before_preview_source_.isEmpty();
    if (!before_preview_source_.isEmpty()) {
        before_preview_source_.clear();
        emit beforePreviewSourceChanged();
    }
    before_requested_ = requested;
    if (requested)
        markHistogramUpdating(EditPreviewKind::NeutralBefore);
}

void EditController::schedulePreview(const int delay_ms) {
    if (!active_) {
        return;
    }
    cancelDetailWarmupForRecipeEdit();
    ++render_revision_;
    // Mark current work unsettled before invalidating the Before URL: QML may
    // synchronously request it again from the source-change notification.
    refreshBeforePreviewContext();
    markHistogramUpdating(EditPreviewKind::Current);
    scheduleDetailRefreshForRecipeEdit(delay_ms);
    if (current_rendering_ || before_rendering_) {
        preview_queued_ = true;
        cancelActivePreview(false);
    }
    // A leading-edge throttle keeps the first response prompt during a drag.
    // Repeated slider events do not postpone that first frame indefinitely.
    if (delay_ms <= 0 || !preview_debounce_.isActive()) {
        preview_debounce_.start(delay_ms);
    }
}

void EditController::cancelActivePreview(const bool force) {
    if (preview_render_token_ == 0) {
        return;
    }
    if (!should_cancel_edit_preview(EditPreviewCancellationState{
            .force = force,
            .current_rendering = current_rendering_,
            .in_flight_policy = in_flight_preview_policy_,
            .gesture_active = !active_parameter_gestures_.isEmpty(),
            .first_interactive_frame_presented =
                first_interactive_frame_presented_,
        })) {
        return;
    }
    (void)backend_->cancelEditPreviewRequest(preview_render_token_);
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


void EditController::emitBusyChange(const bool previous_busy) {
    if (previous_busy != busy()) {
        emit busyChanged();
    }
}
