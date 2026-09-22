#include "edit_controller.hpp"
#include "edit_performance_diagnostics.hpp"

#include <QtConcurrent>

#include <QSize>
#include <QVariantMap>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <utility>

namespace {

constexpr int EDIT_DETAIL_DEBOUNCE_MS = 70;
constexpr int EDIT_DETAIL_WARMUP_IDLE_MS = 80;
constexpr std::uint32_t EDIT_DETAIL_FIRST_REGION_SIDE = 512;
constexpr std::uint32_t EDIT_DETAIL_PROGRESSIVE_VIEWPORT_SIDE = 1'024;

[[nodiscard]] LocalizedUiMessage edit_message(
    const char* const source,
    const std::initializer_list<LocalizedUiArgument> arguments = {}
) {
    return {"EditController", source, arguments};
}

} // namespace

void EditController::requestDetailViewport(
    const double center_x,
    const double center_y,
    const int viewport_width_pixels,
    const int viewport_height_pixels,
    const bool retain_pan_margin
) {
    if (!active_ || crop_tool_active_ || !std::isfinite(center_x) || !std::isfinite(center_y)
        || center_x < 0.0 || center_x > 1.0 || center_y < 0.0 || center_y > 1.0
        || viewport_width_pixels <= 0 || viewport_height_pixels <= 0
        || viewport_width_pixels > 8'192 || viewport_height_pixels > 8'192) {
        return;
    }
    const bool same_request = detail_mode_ && detail_retain_pan_margin_ == retain_pan_margin
        && detail_center_x_ == center_x
        && detail_center_y_ == center_y
        && detail_viewport_width_ == static_cast<std::uint32_t>(viewport_width_pixels)
        && detail_viewport_height_ == static_cast<std::uint32_t>(viewport_height_pixels);
    detail_retain_pan_margin_ = retain_pan_margin;
    detail_center_x_ = center_x;
    detail_center_y_ = center_y;
    detail_viewport_width_ = static_cast<std::uint32_t>(viewport_width_pixels);
    detail_viewport_height_ = static_cast<std::uint32_t>(viewport_height_pixels);
    // A pan inside the already presented rectangle needs only a Scene Graph
    // transform. Do not create a generation, cancel a worker, copy RGB, or
    // restart a debounce timer when no new pixels are needed.
    if (detail_mode_ && settled_render_revision_ == render_revision_
        && detail_full_width_ > 0 && detail_full_height_ > 0
        && detail_tiles_.size() == 1) {
        const auto tile = detail_tiles_.front().toMap();
        const double width = std::min<double>(viewport_width_pixels, detail_full_width_);
        const double height = std::min<double>(viewport_height_pixels, detail_full_height_);
        const double left = std::clamp(std::round(center_x * detail_full_width_ - width / 2),
            0.0, detail_full_width_ - width);
        const double top = std::clamp(std::round(center_y * detail_full_height_ - height / 2),
            0.0, detail_full_height_ - height);
        const double tile_left = tile.value(QStringLiteral("x")).toDouble();
        const double tile_top = tile.value(QStringLiteral("y")).toDouble();
        if (left >= tile_left && top >= tile_top
            && left + width <= tile_left + tile.value(QStringLiteral("width")).toDouble()
            && top + height <= tile_top + tile.value(QStringLiteral("height")).toDouble()) {
            if (detail_queued_ || detail_rendering_) {
                ++detail_viewport_revision_;
                invalidateDetailPresentation(false);
                detail_queued_ = false;
                detail_debounce_.stop();
            }
            return;
        }
    }
    if (same_request && (detail_queued_ || detail_rendering_)) {
        return;
    }
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
    startDisplayScopeTask();
}

void EditController::cancelDetailWarmupForRecipeEdit() {
    // A newly edited Recipe makes an idle full-detail warmup useless. Advance
    // the shared request token before the preview work competes for CPU; the
    // prepared source itself remains reusable, but the old tile render exits
    // at its next cancellation boundary.
    if (!detail_warmup_debounce_.isActive() && !detail_warmup_watcher_.isRunning()) {
        return;
    }
    detail_warmup_debounce_.stop();
    detail_warmup_token_ = backend_->beginEditDetailRequest();
    if (full_resolution_preparing_) {
        setFullResolutionState(false, false, 0);
    }
}

void EditController::scheduleDetailRefreshForRecipeEdit(const int delay_ms) {
    if (!detail_mode_) {
        return;
    }
    invalidateDetailPresentation();
    detail_queued_ = true;
    detail_debounce_.start(std::max(delay_ms, EDIT_DETAIL_DEBOUNCE_MS));
}

void EditController::finishDetailTask() {
    EditDetailTaskResult result = detail_watcher_.result();
    const bool center_phase = std::exchange(detail_center_phase_, false);
    if (persistence_state_.closeAfterAutosave()) {
        detail_queued_ = false;
        before_requested_ = false;
        setDetailRunning(false);
        maybeFinishDeferredApplicationClose();
        return;
    }
    const bool accepted = detail_mode_ && active_
                          && accepts_edit_detail(
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
            const auto* tile =
                result.viewport.tiles.size() == 1 ? &result.viewport.tiles.front() : nullptr;
            const std::uint64_t expected_stride =
                tile == nullptr ? 0U : static_cast<std::uint64_t>(tile->width) * 3U;
            const std::uint64_t expected_bytes =
                tile == nullptr ? 0U : expected_stride * tile->height;
            const bool valid = result.viewport.full_width > 0 && result.viewport.full_height > 0
                               && tile != nullptr && tile->width > 0 && tile->height > 0
                               && tile->row_stride_bytes == expected_stride
                               && expected_bytes == static_cast<std::uint64_t>(tile->bytes.size());
            QVector<EditPreviewStore::DetailPublication> publications;
            QVariantList presentation;
            if (valid) {
                auto& mutable_tile = result.viewport.tiles.front();
                const QString ticket =
                    (center_phase ? QStringLiteral("viewport-center-%1-%2")
                                  : QStringLiteral("viewport-%1-%2"))
                        .arg(mutable_tile.x)
                        .arg(mutable_tile.y);
                publications.push_back({
                    .ticket = ticket,
                    .bytes = std::move(mutable_tile.bytes),
                    .dimensions = QSize(
                        static_cast<int>(mutable_tile.width),
                        static_cast<int>(mutable_tile.height)
                    ),
                    .row_stride_bytes = static_cast<qsizetype>(mutable_tile.row_stride_bytes),
                });
                QVariantMap item;
                item.insert(QStringLiteral("x"), mutable_tile.x);
                item.insert(QStringLiteral("y"), mutable_tile.y);
                item.insert(QStringLiteral("width"), mutable_tile.width);
                item.insert(QStringLiteral("height"), mutable_tile.height);
                item.insert(
                    QStringLiteral("source"),
                    QStringLiteral("image://shadow-edit/detail/%1?photo=%2&recipe=%3&viewport=%4")
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
                const bool geometry_changed =
                    detail_full_width_ != result.viewport.full_width
                    || detail_full_height_ != result.viewport.full_height
                    || detail_retained_bytes_ != result.viewport.retained_bytes;
                detail_full_width_ = result.viewport.full_width;
                detail_full_height_ = result.viewport.full_height;
                detail_retained_bytes_ = result.viewport.retained_bytes;
                preview_store_->publishDetails(std::move(publications), result.generation);
                detail_tiles_ = std::move(presentation);
                if (geometry_changed) {
                    emit detailGeometryChanged();
                }
                setFullResolutionState(false, true, result.viewport.retained_bytes);
                emit detailTilesChanged();
                log_edit_performance_checkpoint(
                    center_phase ? "detail-center-published" : "detail-viewport-published",
                    result.generation.photo,
                    result.generation.recipe_revision,
                    result.viewport.retained_bytes
                );
                // Keep the exact center visible over the retained preview while
                // the complete viewport renders. An edit, pan, or photo switch
                // advances the generation/token and prevents this continuation.
                if (center_phase && detail_mode_ && active_ && !detail_queued_
                    && accepts_edit_detail(
                        result.generation,
                        photo_generation_,
                        render_revision_,
                        detail_viewport_revision_
                    )) {
                    dispatchDetailRender(false);
                    return;
                }
                if (center_phase) {
                    setDetailRunning(false);
                    if (detail_queued_) {
                        maybeStartDetailRender();
                    } else {
                        maybeStartBeforePreview();
                    }
                    maybeFinishDeferredApplicationClose();
                    return;
                }
                const double retained_mib =
                    static_cast<double>(detail_retained_bytes_) / (1'024.0 * 1'024.0);
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

    setDetailRunning(false);
    if (detail_queued_) {
        maybeStartDetailRender();
    } else {
        maybeStartBeforePreview();
    }
    startDisplayScopeTask();
    maybeFinishDeferredApplicationClose();
}

void EditController::startDetailRender() {
    if (!detail_mode_ || !active_ || crop_tool_active_) {
        detail_queued_ = false;
        return;
    }
    if (stateTaskRunning() || current_rendering_ || before_rendering_
        || detail_warmup_watcher_.isRunning()
        || settled_render_revision_ != render_revision_ || detail_rendering_) {
        detail_queued_ = true;
        return;
    }
    detail_queued_ = false;
    setDetailRunning(true);
    if (!full_resolution_ready_) {
        setFullResolutionState(true, false, 0);
    }
    setStatusMessage(
        edit_message(QT_TRANSLATE_NOOP("EditController", "Preparing exact full-resolution detail…"))
    );
    log_edit_performance_checkpoint("detail-requested", photo_generation_, render_revision_);
    dispatchDetailRender(
        detail_viewport_width_ > EDIT_DETAIL_PROGRESSIVE_VIEWPORT_SIDE
        || detail_viewport_height_ > EDIT_DETAIL_PROGRESSIVE_VIEWPORT_SIDE
    );
}

void EditController::dispatchDetailRender(const bool center_first) {
    detail_center_phase_ = center_first;
    // The first region is an exact, bounded 512px readback. The second pass
    // retains the existing tile grid and surrounding pan margin unchanged.
    const std::uint32_t width = center_first
        ? EDIT_DETAIL_FIRST_REGION_SIDE
        : detail_retain_pan_margin_
            ? std::min(8'192U, std::max(1'025U, detail_viewport_width_ + 512U))
            : detail_viewport_width_;
    const std::uint32_t height = center_first
        ? EDIT_DETAIL_FIRST_REGION_SIDE
        : detail_retain_pan_margin_
            ? std::min(8'192U, std::max(1'025U, detail_viewport_height_ + 512U))
            : detail_viewport_height_;
    detail_watcher_.setFuture(
        QtConcurrent::run(
            EditTaskRunner::renderDetail,
            backend_,
            photo_id_,
            source_path_,
            base_commit_id_,
            grade_stack_,
            detail_render_token_,
            detail_center_x_,
            detail_center_y_,
            width,
            height,
            EditDetailGeneration{
                .photo = photo_generation_,
                .recipe_revision = render_revision_,
                .viewport_revision = detail_viewport_revision_,
            }
        )
    );
}

void EditController::scheduleDetailWarmup() {
    const auto& raw_ai_denoise = grade_stack_.raw_ai_denoise;
    if (crop_tool_active_ || detail_mode_ || settled_render_revision_ != render_revision_
        || !work_scheduler_.admitsIdleDetailWarmup(workAdmissionState())
        || !edit_detail_admits_idle_warmup(
            raw_ai_denoise.present,
            raw_ai_denoise.enabled,
            raw_ai_denoise.bypassed
        )) {
        return;
    }
    detail_warmup_debounce_.start(EDIT_DETAIL_WARMUP_IDLE_MS);
}

void EditController::startDetailWarmup() {
    const auto& raw_ai_denoise = grade_stack_.raw_ai_denoise;
    if (crop_tool_active_ || detail_mode_ || settled_render_revision_ != render_revision_
        || !work_scheduler_.admitsIdleDetailWarmup(workAdmissionState())
        || !edit_detail_admits_idle_warmup(
            raw_ai_denoise.present,
            raw_ai_denoise.enabled,
            raw_ai_denoise.bypassed
        )) {
        return;
    }
    // This uses the same global cancellation source as foreground detail.
    // Any later pan, zoom, Recipe edit, or photo switch increments it and
    // causes this idle request to be discarded between tiles.
    detail_warmup_token_ = backend_->beginEditDetailRequest();
    log_edit_performance_checkpoint("detail-warmup-start", photo_generation_, render_revision_);
    if (!full_resolution_ready_) {
        setFullResolutionState(true, false, 0);
    }
    detail_warmup_watcher_.setFuture(
        QtConcurrent::run(
            EditTaskRunner::warmDetailSource,
            backend_,
            photo_id_,
            source_path_,
            base_commit_id_,
            grade_stack_,
            detail_warmup_token_,
            photo_generation_,
            render_revision_
        )
    );
}

void EditController::finishDetailWarmupTask() {
    const EditDetailWarmupTaskResult result = detail_warmup_watcher_.result();
    const bool stale =
        result.photo_generation != photo_generation_ || result.render_revision != render_revision_;
    const bool superseded =
        result.error.startsWith(QStringLiteral("full detail render was superseded"));
    if (stale || superseded) {
        if (full_resolution_preparing_) {
            setFullResolutionState(false, false, 0);
        }
        maybeStartDetailRender();
        startDisplayScopeTask();
        scheduleDetailWarmup();
        return;
    }
    if (result.error.isEmpty()) {
        setFullResolutionState(false, true, result.retained_bytes);
        log_edit_performance_checkpoint(
            "detail-warmup-ready",
            result.photo_generation,
            result.render_revision,
            result.retained_bytes
        );
    } else if (full_resolution_preparing_) {
        // A provider error remains visible only if the user explicitly asks
        // for full detail. The status bar should nevertheless stop reporting
        // active background development.
        setFullResolutionState(false, false, 0);
    }
    maybeStartDetailRender();
    startDisplayScopeTask();
}

void EditController::maybeStartDetailRender() {
    if (!detail_queued_ || !detail_mode_ || detail_rendering_ || detail_debounce_.isActive()) {
        return;
    }
    if (stateTaskRunning() || current_rendering_ || before_rendering_
        || detail_warmup_watcher_.isRunning()
        || settled_render_revision_ != render_revision_) {
        return;
    }
    detail_debounce_.start(0);
}

void EditController::invalidateDetailPresentation(const bool discard_tiles) {
    detail_render_token_ = backend_->beginEditDetailRequest();
    if (discard_tiles) {
        preview_store_->clearDetails(
            EditDetailGeneration{
                .photo = photo_generation_,
                .recipe_revision = render_revision_,
                .viewport_revision = detail_viewport_revision_,
            }
        );
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
    const bool had_geometry =
        detail_full_width_ != 0 || detail_full_height_ != 0 || detail_retained_bytes_ != 0;
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
    if (full_resolution_preparing_ == preparing && full_resolution_ready_ == ready
        && full_resolution_retained_bytes_ == retained_bytes) {
        return;
    }
    full_resolution_preparing_ = preparing;
    full_resolution_ready_ = ready;
    full_resolution_retained_bytes_ = retained_bytes;
    emit fullResolutionStateChanged();
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
