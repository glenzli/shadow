#include "edit_targeted_curve_controller.hpp"
#include "edit_controller.hpp"
#include <QDebug>
#include <QtConcurrent>
#include <algorithm>
#include <cmath>

namespace {
const QString gesture_key = QStringLiteral("tone_curve/targeted");
}

EditTargetedCurveController::EditTargetedCurveController(EditController& owner) : owner_(owner) {
    connect(&owner_.tone_curve_points_, &ToneCurvePointModel::pointsChanged, this, [this] {
        sampled_curve_.clear();
    });
    connect(&owner_, &EditController::pointColorPickerActiveChanged, this, [this] {
        if (owner_.pointColorPickerActive())
            setActive(false);
    });
    connect(&owner_, &EditController::whiteBalancePickerActiveChanged, this, [this] {
        if (owner_.whiteBalancePickerActive())
            setActive(false);
    });
    connect(&owner_, &EditController::rawWhiteBalancePickerActiveChanged, this, [this] {
        if (owner_.rawWhiteBalancePickerActive())
            setActive(false);
    });
    debounce_.setSingleShot(true);
    debounce_.setInterval(100);
    connect(&debounce_, &QTimer::timeout, this, &EditTargetedCurveController::prepare);
    connect(
        &watcher_,
        &QFutureWatcher<TargetedCurveInputResult>::finished,
        this,
        &EditTargetedCurveController::accept
    );
    connect(
        &owner_,
        &EditController::parametersChanged,
        this,
        &EditTargetedCurveController::reconcile
    );
    connect(
        &owner_,
        &EditController::toneCurveChanged,
        this,
        &EditTargetedCurveController::reconcile
    );
    connect(&owner_, &EditController::selectedGradeNodeChanged, this, [this] {
        if (!applying_)
            setActive(false);
    });
    connect(&owner_, &EditController::sourceIdentityChanged, this, [this] { setActive(false); });
    connect(&owner_, &EditController::activeChanged, this, [this] {
        if (!owner_.active())
            setActive(false);
    });
}
EditTargetedCurveController::~EditTargetedCurveController() {
    disconnect(&watcher_, nullptr, this, nullptr);
    debounce_.stop();
    if (token_)
        (void)owner_.backend_->cancelEditPreviewRequest(token_);
    watcher_.waitForFinished();
}
QString EditTargetedCurveController::status() const {
    if (busy())
        return tr("Preparing curve input…");
    if (failed_)
        return tr("Wait for the current preview, then retry");
    if (input_ > 1 || (input_ < 0 && input_ != -1))
        return tr("Sample is outside the editable curve range");
    return tr("Drag on the photo to adjust similar tones · Esc to cancel");
}
BackendGradeStack EditTargetedCurveController::inputKey() const {
    auto key = owner_.grade_stack_;
    const int selected = owner_.selected_grade_node_index_;
    if (selected >= 0 && selected < key.grade_nodes.size()) {
        auto& fine = key.grade_nodes[selected].fine;
        const int channel = owner_.tone_curve_channel_;
        if (channel == 0)
            fine.oklab_lightness_curve_points.clear();
        for (int i = channel >= 2 ? 1 : 0; i < 4; ++i)
            fine.rgb_curve_points[static_cast<std::size_t>(i)].clear();
    }
    return key;
}
void EditTargetedCurveController::setActive(bool active) {
    if (active_ == active)
        return;
    if (active && (!owner_.active() || !owner_.toneCurveEditable() || owner_.interactionLocked()))
        return;
    finish(true);
    active_ = active;
    emit activeChanged();
    if (active) {
        owner_.setPointColorPickerActive(false);
        owner_.setWhiteBalancePickerActive(false);
        owner_.setRawWhiteBalancePickerActive(false);
        owner_.setRetouchPickerActive(false);
        refresh();
    } else {
        ++sequence_;
        if (token_)
            (void)owner_.backend_->cancelEditPreviewRequest(token_);
        debounce_.stop();
        map_ = {};
        input_ = -1;
        emit sampleChanged();
        emit changed();
    }
}
void EditTargetedCurveController::reconcile() {
    if (!active_ || applying_ || dragging())
        return;
    if (channel_ != owner_.tone_curve_channel_ || key_ != inputKey())
        refresh();
}
void EditTargetedCurveController::refresh() {
    if (!active_ || dragging())
        return;
    ++sequence_;
    if (token_)
        (void)owner_.backend_->cancelEditPreviewRequest(token_);
    map_ = {};
    input_ = -1;
    failed_ = false;
    key_ = inputKey();
    channel_ = owner_.tone_curve_channel_;
    if (!watcher_.isRunning())
        debounce_.start();
    emit sampleChanged();
    emit changed();
}
void EditTargetedCurveController::prepare() {
    if (!active_ || watcher_.isRunning())
        return;
    requested_sequence_ = sequence_;
    token_ = owner_.backend_->beginEditPreviewRequest();
    if (!token_) {
        failed_ = true;
        emit changed();
        return;
    }
    const auto backend = owner_.backend_;
    const auto photo = owner_.photo_id_, source = owner_.source_path_,
               base = owner_.base_commit_id_;
    const auto stack = owner_.grade_stack_;
    const int node = owner_.selected_grade_node_index_, channel = owner_.tone_curve_channel_;
    const auto token = token_;
    watcher_.setFuture(
        QtConcurrent::run([backend, photo, source, base, stack, node, channel, token] {
            TargetedCurveInputResult result;
            try {
                result.map =
                    backend->curveInputMap(photo, source, base, stack, token, node, channel);
            } catch (const std::exception& e) {
                result.error = QString::fromUtf8(e.what());
            }
            return result;
        })
    );
    emit changed();
}
void EditTargetedCurveController::accept() {
    token_ = 0;
    if (!active_)
        return;
    if (requested_sequence_ != sequence_) {
        debounce_.start();
        emit changed();
        return;
    }
    auto result = watcher_.result();
    if (!result.error.isEmpty())
        qWarning() << "Curve input sampling failed:" << result.error.left(240);
    const auto& m = result.map;
    if (result.error.isEmpty() && m.width > 0 && m.height > 0 && m.width <= 512 && m.height <= 512
        && m.values.size() == static_cast<qsizetype>(m.width * m.height)
        && std::ranges::all_of(m.values, [](float v) { return std::isfinite(v); }))
        map_ = std::move(result.map);
    failed_ = map_.values.isEmpty();
    emit changed();
}
void EditTargetedCurveController::hover(double x, double y) {
    if (!ready() || dragging() || !std::isfinite(x) || !std::isfinite(y))
        return;
    if (x < 0 || y < 0 || x > 1 || y > 1) {
        input_ = -1;
        emit sampleChanged();
        return;
    }
    const auto px = std::min(map_.width - 1, static_cast<std::uint32_t>(x * map_.width));
    const auto py = std::min(map_.height - 1, static_cast<std::uint32_t>(y * map_.height));
    input_ = map_.values[static_cast<qsizetype>(py * map_.width + px)];
    if (sampled_curve_.isEmpty())
        sampled_curve_ = owner_.tone_curve_points_.sampledPoints(1025, true);
    if (!sampled_curve_.isEmpty())
        output_ =
            sampled_curve_[static_cast<qsizetype>(std::lround(std::clamp(input_, 0.0, 1.0) * 1024))]
                .toPointF()
                .y();
    emit sampleChanged();
    emit changed();
}
bool EditTargetedCurveController::begin(double x, double y) {
    if (!ready() || dragging() || owner_.interactionLocked() || !owner_.toneCurveEditable())
        return false;
    reconcile();
    if (!ready())
        return false;
    hover(x, y);
    if (input_ < 0 || input_ > 1)
        return false;
    owner_.finishActiveGesture();
    before_ = owner_.grade_stack_;
    photo_generation_ = owner_.photo_generation_;
    point_ = -1;
    const auto points = owner_.tone_curve_points_.points();
    for (int i = 0; i < points.size(); ++i)
        if (std::abs(points[i].x - input_) < 0.015) {
            point_ = i;
            input_ = points[i].x;
            output_ = points[i].y;
            break;
        }
    origin_output_ = output_;
    owner_.beginParameterEdit(gesture_key);
    if (!owner_.active_parameter_gestures_.contains(gesture_key)) {
        before_.reset();
        return false;
    }
    owner_.persistence_state_.stopAutosaveDebounce();
    emit changed();
    emit sampleChanged();
    return true;
}
void EditTargetedCurveController::move(double delta) {
    if (!before_ || photo_generation_ != owner_.photo_generation_ || !std::isfinite(delta)
        || (point_ < 0 && std::abs(delta) < 1e-6))
        return;
    applying_ = true;
    if (point_ < 0)
        point_ = owner_.tone_curve_points_.addPoint(input_, origin_output_);
    if (point_ >= 0) {
        output_ = std::clamp(origin_output_ - delta, 0.0, 1.0);
        (void)owner_.tone_curve_points_.movePoint(point_, input_, output_);
        auto& fine = owner_.grade_stack_.grade_nodes[owner_.selected_grade_node_index_].fine;
        auto& values = channel_ == 0
                           ? fine.oklab_lightness_curve_points
                           : fine.rgb_curve_points[static_cast<std::size_t>(channel_ - 1)];
        values.clear();
        for (const auto point : owner_.tone_curve_points_.points()) {
            values.push_back(point.x);
            values.push_back(point.y);
        }
        emit owner_.toneCurveChanged();
        owner_.setDirty(
            owner_.version_draft_ || owner_.grade_stack_ != owner_.committed_grade_stack_
        );
        owner_.schedulePreview(16);
    }
    applying_ = false;
    emit sampleChanged();
}
void EditTargetedCurveController::finish(bool cancel) {
    if (!before_)
        return;
    auto before = std::move(*before_);
    before_.reset();
    if (photo_generation_ != owner_.photo_generation_)
        return;
    applying_ = true;
    const bool edited = owner_.grade_stack_ != before;
    if (cancel)
        owner_.setGradeStack(std::move(before));
    owner_.endParameterEdit(gesture_key);
    if (edited && !cancel) {
        ++owner_.working_revision_;
        owner_.persistence_state_.requestAutosave();
        owner_.clearAutosaveFailure();
    }
    if (owner_.persistence_state_.autosaveRequested() && !owner_.stateTaskRunning())
        owner_.scheduleAutosave();
    applying_ = false;
    emit changed();
}
