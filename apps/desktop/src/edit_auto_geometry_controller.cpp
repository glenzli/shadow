#include "edit_auto_geometry_controller.hpp"

#include "edit_controller.hpp"

#include <QtConcurrent>

#include <QByteArray>

#include <algorithm>
#include <cmath>
#include <exception>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <utility>

namespace {

[[nodiscard]] LocalizedUiMessage auto_geometry_message(
    const char* const source,
    const std::initializer_list<LocalizedUiArgument> arguments = {}
) {
    return {"EditController", source, arguments};
}

[[nodiscard]] bool analysis_geometry_is_compatible(const BackendPhotoGeometry& geometry) noexcept {
    // The current renderer crops before applying the fine homography. Analyze
    // only the complete oriented photo so the transient proposal and accepted
    // Recipe have the same projective center. Quarter-turns and flips are
    // safe because the displayed preview already contains them.
    return geometry.crop_left == 0.0 && geometry.crop_top == 0.0 && geometry.crop_right == 1.0
           && geometry.crop_bottom == 1.0 && geometry.straighten_degrees == 0.0
           && geometry.perspective_vertical == 0.0 && geometry.perspective_horizontal == 0.0;
}

[[nodiscard]] std::optional<shadow::image::AutoGeometryMode> analysis_mode(const int mode) {
    switch (mode) {
    case 0:
        return shadow::image::AutoGeometryMode::automatic;
    case 1:
        return shadow::image::AutoGeometryMode::level;
    case 2:
        return shadow::image::AutoGeometryMode::vertical;
    case 3:
        return shadow::image::AutoGeometryMode::full;
    default:
        return std::nullopt;
    }
}

[[nodiscard]] EditAutoGeometryTaskResult execute_analysis(
    EditPreviewStore::Snapshot snapshot,
    const shadow::image::AutoGeometryMode mode,
    EditAutoGeometryTaskResult result
) {
    try {
        std::span<const std::uint8_t> pixels;
        if (snapshot.frame != nullptr) {
            pixels = snapshot.frame->materializeRgb8();
        } else {
            pixels = std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(snapshot.bytes.constData()),
                static_cast<std::size_t>(snapshot.bytes.size())
            );
        }
        result.proposal = shadow::image::analyze_auto_geometry(
            {
                .pixels = pixels,
                .width = static_cast<std::uint32_t>(snapshot.dimensions.width()),
                .height = static_cast<std::uint32_t>(snapshot.dimensions.height()),
                .row_stride_bytes = static_cast<std::size_t>(snapshot.row_stride_bytes),
            },
            mode
        );
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

} // namespace

EditAutoGeometryController::EditAutoGeometryController(EditController& owner) : owner_(owner) {
    QObject::connect(
        &watcher_,
        &QFutureWatcher<EditAutoGeometryTaskResult>::finished,
        &owner_,
        [this] { finishAnalysis(); }
    );
}

EditAutoGeometryController::~EditAutoGeometryController() {
    QObject::disconnect(&watcher_, nullptr, &owner_, nullptr);
    ++request_sequence_;
    watcher_.waitForFinished();
}

bool EditAutoGeometryController::busy() const noexcept {
    return watcher_.isRunning();
}

bool EditAutoGeometryController::canAnalyze() const noexcept {
    return owner_.active_ && owner_.crop_tool_active_ && !owner_.interactionLocked()
           && !owner_.current_rendering_ && !owner_.before_rendering_ && !watcher_.isRunning()
           && !owner_.preview_debounce_.isActive() && owner_.active_parameter_gestures_.isEmpty()
           && !has_proposal_ && owner_.grade_stack_.geometry.present
           && analysis_geometry_is_compatible(owner_.grade_stack_.geometry);
}

bool EditAutoGeometryController::hasProposal() const noexcept {
    return has_proposal_;
}

bool EditAutoGeometryController::previewing() const noexcept {
    return preview_ready_;
}

int EditAutoGeometryController::confidencePercent() const noexcept {
    return static_cast<int>(std::lround(std::clamp(proposal_.confidence, 0.0, 1.0) * 100.0));
}

double EditAutoGeometryController::suggestedStraightenDegrees() const noexcept {
    return proposal_.straighten_degrees;
}

double EditAutoGeometryController::suggestedPerspectiveVertical() const noexcept {
    return proposal_.perspective_vertical;
}

double EditAutoGeometryController::suggestedPerspectiveHorizontal() const noexcept {
    return proposal_.perspective_horizontal;
}

int EditAutoGeometryController::supportingLines() const noexcept {
    return static_cast<int>(std::min<std::uint32_t>(
        proposal_.supporting_lines,
        static_cast<std::uint32_t>(std::numeric_limits<int>::max())
    ));
}

QString EditAutoGeometryController::statusText() const {
    return status_message_.translated();
}

void EditAutoGeometryController::analyze(const int mode_value) {
    const auto mode = analysis_mode(mode_value);
    if (!mode.has_value() || !canAnalyze()) {
        return;
    }
    const auto snapshot =
        owner_.preview_store_->snapshot(EditPreviewSlot::Current, owner_.render_revision_);
    if ((!snapshot.frame && snapshot.bytes.isEmpty()) || !snapshot.dimensions.isValid()
        || snapshot.dimensions.isEmpty() || snapshot.row_stride_bytes <= 0) {
        status_message_ = auto_geometry_message(QT_TRANSLATE_NOOP(
            "EditController",
            "Wait for the current preview before analyzing geometry"
        ));
        emit owner_.autoGeometryChanged();
        return;
    }

    const bool previous_owner_busy = owner_.busy();
    ++request_sequence_;
    status_message_ = auto_geometry_message(
        QT_TRANSLATE_NOOP("EditController", "Analyzing lines and perspective…")
    );
    watcher_.setFuture(
        QtConcurrent::run(
            execute_analysis,
            snapshot,
            *mode,
            EditAutoGeometryTaskResult{
                .photo_id = owner_.photo_id_,
                .photo_generation = owner_.photo_generation_,
                .render_revision = owner_.render_revision_,
                .request_sequence = request_sequence_,
                .source_geometry = owner_.grade_stack_.geometry,
            }
        )
    );
    publishChange(previous_owner_busy);
}

void EditAutoGeometryController::finishAnalysis() {
    const bool previous_owner_busy = true;
    EditAutoGeometryTaskResult result = watcher_.result();
    if (!resultContextIsCurrent(result)) {
        status_message_.clear();
        publishChange(previous_owner_busy);
        return;
    }
    if (!result.error.isEmpty()) {
        status_message_ = auto_geometry_message(
            QT_TRANSLATE_NOOP("EditController", "Automatic geometry analysis failed · %1"),
            {result.error}
        );
        publishChange(previous_owner_busy);
        return;
    }
    proposal_ = result.proposal;
    if (!proposal_.available) {
        status_message_ = auto_geometry_message(QT_TRANSLATE_NOOP(
            "EditController",
            "No reliable level or perspective correction was found"
        ));
        publishChange(previous_owner_busy);
        return;
    }

    preview_geometry_ = result.source_geometry;
    preview_geometry_.straighten_degrees = proposal_.straighten_degrees;
    preview_geometry_.perspective_vertical = proposal_.perspective_vertical;
    preview_geometry_.perspective_horizontal = proposal_.perspective_horizontal;
    has_proposal_ = true;
    preview_ready_ = false;
    status_message_ = auto_geometry_message(
        QT_TRANSLATE_NOOP(
            "EditController",
            "Rendering automatic geometry preview · %1% confidence"
        ),
        {confidencePercent()}
    );
    publishChange(previous_owner_busy);
    // A transient proposal still needs its own preview generation so the
    // image provider cannot reuse the authoritative frame's URL/cache entry.
    owner_.schedulePreview(0);
    proposal_render_revision_ = owner_.render_revision_;
}

void EditAutoGeometryController::accept() {
    if (!has_proposal_ || !preview_ready_ || owner_.interactionLocked()
        || !analysis_geometry_is_compatible(owner_.grade_stack_.geometry)) {
        return;
    }
    const BackendGradeStack before = owner_.grade_stack_;
    owner_.grade_stack_.geometry.straighten_degrees = preview_geometry_.straighten_degrees;
    owner_.grade_stack_.geometry.perspective_vertical = preview_geometry_.perspective_vertical;
    owner_.grade_stack_.geometry.perspective_horizontal = preview_geometry_.perspective_horizontal;
    has_proposal_ = false;
    preview_ready_ = false;
    proposal_render_revision_ = 0U;
    proposal_ = {};
    status_message_.clear();
    emit owner_.autoGeometryChanged();
    owner_.setFullResolutionState(false, false, 0);
    owner_.parameterEdited(QStringLiteral("geometry/auto"), before);
}

void EditAutoGeometryController::cancel() {
    if (!has_proposal_ && !preview_ready_ && status_message_.isEmpty()) {
        return;
    }
    ++request_sequence_;
    const bool remove_preview = has_proposal_;
    has_proposal_ = false;
    preview_ready_ = false;
    proposal_render_revision_ = 0U;
    proposal_ = {};
    status_message_.clear();
    emit owner_.autoGeometryChanged();
    if (remove_preview && owner_.active_) {
        owner_.cancelActivePreview(true);
        owner_.schedulePreview(0);
    }
}

void EditAutoGeometryController::resetContext() {
    ++request_sequence_;
    has_proposal_ = false;
    preview_ready_ = false;
    proposal_render_revision_ = 0U;
    proposal_ = {};
    status_message_.clear();
    emit owner_.autoGeometryChanged();
}

void EditAutoGeometryController::retranslateUi() {
    if (!status_message_.isEmpty()) {
        emit owner_.autoGeometryChanged();
    }
}

bool EditAutoGeometryController::applyPreviewOverride(
    BackendPhotoGeometry& geometry
) const noexcept {
    if (!has_proposal_) {
        return false;
    }
    geometry.straighten_degrees = preview_geometry_.straighten_degrees;
    geometry.perspective_vertical = preview_geometry_.perspective_vertical;
    geometry.perspective_horizontal = preview_geometry_.perspective_horizontal;
    return true;
}

void EditAutoGeometryController::handlePreviewSettled(const std::uint64_t render_revision) {
    if (!has_proposal_ || render_revision != proposal_render_revision_) {
        return;
    }
    preview_ready_ = true;
    status_message_ = auto_geometry_message(
        QT_TRANSLATE_NOOP("EditController", "Previewing automatic geometry · %1% confidence"),
        {confidencePercent()}
    );
    emit owner_.autoGeometryChanged();
}

void EditAutoGeometryController::handlePreviewFailed(
    const std::uint64_t render_revision,
    const QString& error
) {
    if (!has_proposal_ || render_revision != proposal_render_revision_) {
        return;
    }
    preview_ready_ = false;
    status_message_ = auto_geometry_message(
        QT_TRANSLATE_NOOP("EditController", "Automatic geometry preview failed · %1"),
        {error}
    );
    emit owner_.autoGeometryChanged();
}

void EditAutoGeometryController::publishChange(const bool previous_owner_busy) {
    emit owner_.autoGeometryChanged();
    owner_.emitBusyChange(previous_owner_busy);
}

bool EditAutoGeometryController::resultContextIsCurrent(
    const EditAutoGeometryTaskResult& result
) const noexcept {
    return result.request_sequence == request_sequence_ && owner_.active_
           && result.photo_id == owner_.photo_id_
           && result.photo_generation == owner_.photo_generation_
           && result.render_revision == owner_.render_revision_
           && result.source_geometry == owner_.grade_stack_.geometry;
}

bool EditController::autoGeometryBusy() const noexcept {
    return auto_geometry_controller_ && auto_geometry_controller_->busy();
}

bool EditController::autoGeometryCanAnalyze() const noexcept {
    return auto_geometry_controller_ && auto_geometry_controller_->canAnalyze();
}

bool EditController::autoGeometryHasProposal() const noexcept {
    return auto_geometry_controller_ && auto_geometry_controller_->hasProposal();
}

bool EditController::autoGeometryPreviewing() const noexcept {
    return auto_geometry_controller_ && auto_geometry_controller_->previewing();
}

int EditController::autoGeometryConfidence() const noexcept {
    return auto_geometry_controller_ ? auto_geometry_controller_->confidencePercent() : 0;
}

double EditController::autoGeometrySuggestedStraighten() const noexcept {
    return auto_geometry_controller_ ? auto_geometry_controller_->suggestedStraightenDegrees()
                                     : 0.0;
}

double EditController::autoGeometrySuggestedVertical() const noexcept {
    return auto_geometry_controller_ ? auto_geometry_controller_->suggestedPerspectiveVertical()
                                     : 0.0;
}

double EditController::autoGeometrySuggestedHorizontal() const noexcept {
    return auto_geometry_controller_ ? auto_geometry_controller_->suggestedPerspectiveHorizontal()
                                     : 0.0;
}

int EditController::autoGeometrySupportingLines() const noexcept {
    return auto_geometry_controller_ ? auto_geometry_controller_->supportingLines() : 0;
}

QString EditController::autoGeometryStatusText() const {
    return auto_geometry_controller_ ? auto_geometry_controller_->statusText() : QString{};
}

void EditController::analyzeAutoGeometry(const int mode) {
    if (auto_geometry_controller_) {
        auto_geometry_controller_->analyze(mode);
    }
}

void EditController::acceptAutoGeometry() {
    if (auto_geometry_controller_) {
        auto_geometry_controller_->accept();
    }
}

void EditController::cancelAutoGeometry() {
    if (auto_geometry_controller_) {
        auto_geometry_controller_->cancel();
    }
}
