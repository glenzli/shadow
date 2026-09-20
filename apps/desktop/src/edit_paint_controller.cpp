#include "edit_paint_controller.hpp"
#include "edit_controller.hpp"
#include "edit_liquify_coordinates.hpp"
#include <QImage>
#include <QUuid>
#include <QVariantMap>
#include <algorithm>
#include <cmath>

namespace {
const QString stroke_key = QStringLiteral("paint/stroke");
}
EditPaintController::EditPaintController(EditController& owner) : QObject(&owner), owner_(owner) {
    connect(&owner_, &EditController::parametersChanged, this, &EditPaintController::changed);
    connect(&owner_, &EditController::stateBusyChanged, this, &EditPaintController::changed);
    connect(&owner_, &EditController::sourceIdentityChanged, this, [this] {
        before_.reset();
        stroke_index_ = -1;
        selected_id_.clear();
        status_.clear();
        picking_ = false;
        emit changed();
        emit brushChanged();
    });
}
int EditPaintController::selectedIndex() const {
    const auto& layers = owner_.grade_stack_.paint_layers;
    for (qsizetype i = 0; i < layers.size(); ++i)
        if (layers[i].id == selected_id_)
            return int(i);
    return layers.isEmpty() ? -1 : int(layers.size() - 1);
}
const BackendPaintLayer* EditPaintController::layer() const {
    const int i = selectedIndex();
    return i < 0 ? nullptr : &owner_.grade_stack_.paint_layers[i];
}
QVariantList EditPaintController::layers() const {
    QVariantList result;
    for (const auto& layer : owner_.grade_stack_.paint_layers)
        result.push_back(
            QVariantMap{
                {QStringLiteral("id"), layer.id},
                {QStringLiteral("label"),
                 (layer.label.isEmpty() || layer.label == QStringLiteral("Paint layer"))
                     ? tr("Paint layer")
                     : layer.label},
                {QStringLiteral("enabled"), layer.enabled},
                {QStringLiteral("strokes"), layer.strokes.size()}
            }
        );
    return result;
}
bool EditPaintController::editable() const {
    return owner_.active_ && !owner_.interactionLocked();
}
bool EditPaintController::canPaint() const {
    const auto* l = layer();
    return editable() && owner_.level_zero_width_ > 0 && owner_.level_zero_height_ > 0
           && (!l || l->enabled);
}
bool EditPaintController::layerEnabled() const {
    const auto* l = layer();
    return l && l->enabled;
}
double EditPaintController::layerOpacity() const {
    const auto* l = layer();
    return l ? l->opacity : 1;
}
int EditPaintController::blend() const {
    const auto* l = layer();
    return l ? l->blend : 1;
}
BackendPaintLayer EditPaintController::freshLayer() const {
    BackendPaintLayer result;
    result.label = QStringLiteral("Paint layer");
    result.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    result.coordinate_width = owner_.level_zero_width_;
    result.coordinate_height = owner_.level_zero_height_;
    return result;
}
void EditPaintController::activate() {
    if (!editable())
        return;
    owner_.finishActiveGesture();
    owner_.selected_recipe_node_kind_ = QStringLiteral("paint");
    owner_.selected_point_color_index_ = -1;
    owner_.clearPointColorScopeReference();
    emit owner_.selectedGradeNodeChanged();
    emit owner_.gradeNodeActionsChanged();
    emit owner_.gradeNodeEnabledChanged();
    owner_.notifyParametersChanged();
}
void EditPaintController::selectLayer(int index) {
    if (!editable() || index < 0 || index >= owner_.grade_stack_.paint_layers.size())
        return;
    owner_.finishActiveGesture();
    selected_id_ = owner_.grade_stack_.paint_layers[index].id;
    emit changed();
}
void EditPaintController::addLayer() {
    if (!editable() || owner_.grade_stack_.paint_layers.size() >= 8 || !owner_.level_zero_width_)
        return;
    owner_.finishActiveGesture();
    const auto before = owner_.grade_stack_;
    auto next = freshLayer();
    selected_id_ = next.id;
    owner_.grade_stack_.paint_layers.push_back(next);
    owner_.parameterEdited(QStringLiteral("paint/layer/add"), before);
}
void EditPaintController::removeLayer() {
    if (!editable() || !layer())
        return;
    owner_.finishActiveGesture();
    const auto before = owner_.grade_stack_;
    owner_.grade_stack_.paint_layers.removeAt(selectedIndex());
    selected_id_.clear();
    owner_.parameterEdited(QStringLiteral("paint/layer/remove"), before);
}
void EditPaintController::moveLayer(int offset) {
    if (!editable() || !layer() || (offset != -1 && offset != 1))
        return;
    owner_.finishActiveGesture();
    const int index = selectedIndex(), next = index + offset;
    if (next < 0 || next >= owner_.grade_stack_.paint_layers.size())
        return;
    const auto before = owner_.grade_stack_;
    selected_id_ = layer()->id;
    owner_.grade_stack_.paint_layers.move(index, next);
    owner_.parameterEdited(QStringLiteral("paint/layer/move"), before);
}
void EditPaintController::editLayer(
    const QString& key,
    const std::function<void(BackendPaintLayer&)>& edit
) {
    if (!editable() || !layer() || strokeActive())
        return;
    const auto before = owner_.grade_stack_;
    edit(owner_.grade_stack_.paint_layers[selectedIndex()]);
    if (before != owner_.grade_stack_)
        owner_.parameterEdited(key, before);
}
void EditPaintController::setLayerEnabled(bool value) {
    editLayer(QStringLiteral("paint/layer/enabled"), [value](auto& l) { l.enabled = value; });
}
void EditPaintController::setLayerOpacity(double value) {
    if (std::isfinite(value) && value >= 0 && value <= 1)
        editLayer(QStringLiteral("paint/layer/opacity"), [value](auto& l) { l.opacity = value; });
}
void EditPaintController::setBlend(int value) {
    if (value >= 0 && value <= 2)
        editLayer(QStringLiteral("paint/layer/blend"), [value](auto& l) {
            l.blend = std::uint8_t(value);
        });
}
void EditPaintController::setRadius(double v) {
    if (std::isfinite(v) && v >= 0.0001 && v <= 0.25 && !strokeActive()) {
        brush_.radius = v;
        emit brushChanged();
    }
}
void EditPaintController::setHardness(double v) {
    if (std::isfinite(v) && v >= 0 && v <= 1 && !strokeActive()) {
        brush_.hardness = v;
        emit brushChanged();
    }
}
void EditPaintController::setOpacity(double v) {
    if (std::isfinite(v) && v >= 0 && v <= 1 && !strokeActive()) {
        brush_.opacity = v;
        emit brushChanged();
    }
}
void EditPaintController::setFlow(double v) {
    if (std::isfinite(v) && v >= 0 && v <= 1 && !strokeActive()) {
        brush_.flow = v;
        emit brushChanged();
    }
}
void EditPaintController::setColor(QColor v) {
    if (v.isValid() && !strokeActive()) {
        brush_.red = v.redF();
        brush_.green = v.greenF();
        brush_.blue = v.blueF();
        emit brushChanged();
    }
}
void EditPaintController::setErase(bool v) {
    if (!strokeActive()) {
        brush_.erase = v;
        picking_ = false;
        emit brushChanged();
    }
}
void EditPaintController::setPicking(bool v) {
    if (!strokeActive()) {
        picking_ = v;
        emit brushChanged();
    }
}

double EditPaintController::displayRadius(double aspect) const {
    const auto geometry = owner_.grade_stack_.geometry.enabled ? owner_.grade_stack_.geometry
                                                               : BackendPhotoGeometry{};
    // Local differential at the centre handles crop, orientation and straighten.
    const auto a = EditLiquifyCoordinates::originalPointForOutput({0.5, 0.5}, aspect, geometry);
    const auto b = EditLiquifyCoordinates::originalPointForOutput({0.501, 0.5}, aspect, geometry);
    if (!a || !b)
        return radius();
    const double scale = std::hypot(
        (b->x() - a->x()) * owner_.level_zero_width_,
        (b->y() - a->y()) * owner_.level_zero_height_
    );
    return scale > 0 ? radius() * std::min(owner_.level_zero_width_, owner_.level_zero_height_)
                           * 0.001 / scale
                     : radius();
}
bool EditPaintController::beginStroke(double x, double y, double aspect) {
    if (!canPaint() || strokeActive() || !std::isfinite(aspect) || aspect <= 0 || !std::isfinite(x)
        || !std::isfinite(y))
        return false;
    owner_.finishActiveGesture();
    const auto* l = layer();
    qsizetype points = 0;
    if (l)
        for (const auto& s : l->strokes)
            points += s.points.size();
    if (l && (l->strokes.size() >= 128 || points >= 32768)) {
        status_ = tr("This layer is full. Add a new paint layer to continue.");
        emit changed();
        return false;
    }
    generation_ = owner_.photo_generation_;
    aspect_ = aspect;
    stroke_distance_ = 0;
    double used_dabs = 0;
    if (l)
        for (const auto& s : l->strokes) {
            double distance = 0;
            for (qsizetype i = 1; i < s.points.size(); ++i)
                distance += std::hypot(
                    (s.points[i].x - s.points[i - 1].x) * l->coordinate_width,
                    (s.points[i].y - s.points[i - 1].y) * l->coordinate_height
                );
            used_dabs +=
                1
                + distance
                      / (s.radius * 0.25 * std::min(l->coordinate_width, l->coordinate_height));
        }
    dab_budget_ = std::min(32760.0, 262144.0 - used_dabs);
    if (dab_budget_ < 2) {
        status_ = tr("This layer is full. Add a new paint layer to continue.");
        emit changed();
        return false;
    }
    // Prepare the exact inverse deformation once per gesture, never per sample.
    prepared_liquify_ = {};
    if (owner_.grade_stack_.liquify_enabled && !owner_.grade_stack_.liquify_strokes.isEmpty()) {
        shadow::image::PhotoLiquify liquify;
        for (const auto& s : owner_.grade_stack_.liquify_strokes) {
            std::vector<shadow::image::PhotoLiquifyPoint> points;
            for (const auto& p : s.points)
                points.push_back({p.x, p.y, p.pressure});
            if (s.kind == 1)
                liquify.strokes.push_back(
                    shadow::image::PhotoLiquifyReconstructStroke{
                        std::move(points),
                        s.radius,
                        s.strength,
                        s.hardness
                    }
                );
            else
                liquify.strokes.push_back(
                    shadow::image::PhotoLiquifyPushStroke{
                        std::move(points),
                        s.radius,
                        s.strength,
                        s.hardness
                    }
                );
        }
        try {
            prepared_liquify_ = shadow::image::prepare_photo_liquify(
                {owner_.level_zero_width_, owner_.level_zero_height_},
                liquify
            );
        } catch (const std::exception&) {
            status_ = tr("Could not map the paint stroke through Liquify.");
            emit changed();
            return false;
        }
    }
    owner_.beginParameterEdit(stroke_key);
    if (!owner_.active_parameter_gestures_.contains(stroke_key))
        return false;
    before_ = owner_.grade_stack_;
    owner_.persistence_state_.stopAutosaveDebounce();
    if (!layer()) {
        auto next = freshLayer();
        selected_id_ = next.id;
        owner_.grade_stack_.paint_layers.push_back(next);
    }
    selected_id_ = layer()->id;
    auto& target = owner_.grade_stack_.paint_layers[selectedIndex()];
    stroke_index_ = int(target.strokes.size());
    target.strokes.push_back(brush_);
    status_.clear();
    appendPoint(x, y);
    emit changed();
    return true;
}
void EditPaintController::appendPoint(double x, double y, double pressure) {
    if (!before_ || generation_ != owner_.photo_generation_ || !owner_.active_ || !std::isfinite(x)
        || !std::isfinite(y) || !std::isfinite(pressure))
        return;
    const auto geometry = owner_.grade_stack_.geometry.enabled ? owner_.grade_stack_.geometry
                                                               : BackendPhotoGeometry{};
    const auto mapped = EditLiquifyCoordinates::originalPointForOutput(
        {std::clamp(x, 0.0, 1.0), std::clamp(y, 0.0, 1.0)},
        aspect_,
        geometry
    );
    if (!mapped)
        return;
    const auto point = shadow::image::photo_liquify_source_point(
        prepared_liquify_,
        {mapped->x(), mapped->y(), std::clamp(pressure, 0.0, 1.0)}
    );
    auto& target = owner_.grade_stack_.paint_layers[selectedIndex()];
    auto& points = target.strokes[stroke_index_].points;
    qsizetype total = 0;
    for (const auto& s : target.strokes)
        total += s.points.size();
    if (points.size() >= 2048 || total >= 32768) {
        status_ = tr("Stroke limit reached. Release the pointer to finish.");
        emit changed();
        return;
    }
    const BackendPaintPoint next{
        std::clamp(point.x, 0.0, 1.0),
        std::clamp(point.y, 0.0, 1.0),
        point.pressure
    };
    if (!points.isEmpty()) {
        const auto& last = points.last();
        const double distance = std::hypot(
            (next.x - last.x) * target.coordinate_width,
            (next.y - last.y) * target.coordinate_height
        );
        if (distance
            < radius() * std::min(target.coordinate_width, target.coordinate_height) * 0.12)
            return;
        if (1
                + (stroke_distance_ + distance)
                      / (radius() * 0.25
                         * std::min(target.coordinate_width, target.coordinate_height))
            > dab_budget_) {
            status_ = tr("Stroke limit reached. Release the pointer to finish.");
            emit changed();
            return;
        }
        stroke_distance_ += distance;
    }
    points.push_back(next);
    owner_.setFullResolutionState(false, false, 0);
    owner_.notifyParametersChanged();
    owner_.setDirty(owner_.version_draft_ || owner_.grade_stack_ != owner_.committed_grade_stack_);
    owner_.schedulePreview(32);
}
void EditPaintController::finishStroke() {
    if (!before_)
        return;
    if (generation_ != owner_.photo_generation_) {
        before_.reset();
        stroke_index_ = -1;
        return;
    }
    before_.reset();
    stroke_index_ = -1;
    owner_.endParameterEdit(stroke_key);
    ++owner_.working_revision_;
    owner_.persistence_state_.requestAutosave();
    owner_.clearAutosaveFailure();
    if (!owner_.stateTaskRunning())
        owner_.scheduleAutosave();
    emit changed();
}
void EditPaintController::cancelStroke() {
    if (!before_)
        return;
    auto before = std::move(*before_);
    before_.reset();
    stroke_index_ = -1;
    if (generation_ != owner_.photo_generation_)
        return;
    owner_.setGradeStack(std::move(before));
    owner_.endParameterEdit(stroke_key);
    if (owner_.persistence_state_.autosaveRequested())
        owner_.scheduleAutosave();
    emit changed();
}
bool EditPaintController::sampleColor(double x, double y, const QString& generation) {
    if (!editable() || !std::isfinite(x) || !std::isfinite(y))
        return false;
    bool valid = false;
    const auto id = generation.toULongLong(&valid);
    const auto snapshot = valid ? owner_.preview_store_->snapshot(EditPreviewSlot::Current, id)
                                : EditPreviewStore::Snapshot{};
    QImage image;
    try {
        if (snapshot.frame) {
            const auto pixels = snapshot.frame->materializeRgb8();
            image = QImage(
                pixels.data(),
                snapshot.dimensions.width(),
                snapshot.dimensions.height(),
                qsizetype(snapshot.frame->rowStrideBytes()),
                QImage::Format_RGB888
            );
        } else if (
            snapshot.row_stride_bytes > 0
            && snapshot.bytes.size() >= snapshot.row_stride_bytes * snapshot.dimensions.height()
        ) {
            image = QImage(
                reinterpret_cast<const uchar*>(snapshot.bytes.constData()),
                snapshot.dimensions.width(),
                snapshot.dimensions.height(),
                snapshot.row_stride_bytes,
                QImage::Format_RGB888
            );
        } else
            image = QImage::fromData(snapshot.bytes);
    } catch (const std::exception&) {
        image = {};
    }
    if (image.isNull()) {
        status_ = tr("Wait for the preview, then sample again.");
        emit changed();
        return false;
    }
    const int px = std::clamp(int(x * image.width()), 0, image.width() - 1),
              py = std::clamp(int(y * image.height()), 0, image.height() - 1);
    double r = 0, g = 0, b = 0, n = 0;
    for (int yy = std::max(0, py - 1); yy <= std::min(image.height() - 1, py + 1); ++yy)
        for (int xx = std::max(0, px - 1); xx <= std::min(image.width() - 1, px + 1); ++xx) {
            const auto c = image.pixelColor(xx, yy);
            r += c.redF();
            g += c.greenF();
            b += c.blueF();
            ++n;
        }
    setColor(QColor::fromRgbF(float(r / n), float(g / n), float(b / n)));
    setPicking(false);
    status_.clear();
    emit changed();
    return true;
}
