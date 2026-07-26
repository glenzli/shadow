#include "edit_controller.hpp"

#include "edit_stack.hpp"

#include <QtConcurrent>

#include <QCoreApplication>
#include <QColor>
#include <QEvent>
#include <QImage>
#include <QFileInfo>
#include <QJsonDocument>
#include <QUuid>

#include <QSize>
#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace {

// This is Shadow's resident editing proxy, not the full-resolution detail
// source. 768px is too aggressive for modern high-resolution RAWs: Bayer
// phase-preserving downsampling can turn a Z9 frame into a ~690px proxy.
// 1536px is still practical for interactive grading while preserving enough
// texture, edges and colour detail for a useful editing view.
constexpr int EDIT_PREVIEW_THROTTLE_MS = 16;
constexpr int MAX_POINT_COLOR_COUNT = 16;
constexpr auto NODE_MASK_ASSETS_SETTINGS_KEY = "precision/node_mask_assets_v1";
constexpr int MAX_NODE_MASK_ASSET_COUNT = 128;
constexpr int MAX_NODE_MASK_ASSET_NAME_LENGTH = 96;

[[nodiscard]] int point_color_count(const BackendFineEditParameters& fine) noexcept {
    return (fine.color_range_enabled || !fine.additional_point_colors.isEmpty() ? 1 : 0)
        + static_cast<int>(fine.additional_point_colors.size());
}

[[nodiscard]] std::optional<QVariantMap> normalized_node_mask_asset(
    const QVariantMap& source
) {
    const QString id = source.value(QStringLiteral("id")).toString();
    const QString name = source.value(QStringLiteral("name")).toString().trimmed();
    if (QUuid(id).isNull() || name.isEmpty()
        || name.size() > MAX_NODE_MASK_ASSET_NAME_LENGTH) {
        return std::nullopt;
    }

    bool kind_ok = false;
    const int kind = source.value(QStringLiteral("kind")).toInt(&kind_ok);
    if (!kind_ok || kind < 1 || kind > 3) {
        return std::nullopt;
    }

    const auto unit_value = [&source](const QString& key) -> std::optional<double> {
        bool converted = false;
        const double value = source.value(key).toDouble(&converted);
        if (!converted || !std::isfinite(value) || value < 0.0 || value > 1.0) {
            return std::nullopt;
        }
        return value;
    };
    const auto x0 = unit_value(QStringLiteral("x0"));
    const auto y0 = unit_value(QStringLiteral("y0"));
    const auto x1 = unit_value(QStringLiteral("x1"));
    const auto y1 = unit_value(QStringLiteral("y1"));
    const auto radius_x = unit_value(QStringLiteral("radiusX"));
    const auto radius_y = unit_value(QStringLiteral("radiusY"));
    const auto feather = unit_value(QStringLiteral("feather"));
    if (!x0 || !y0 || !x1 || !y1 || !radius_x || !radius_y || !feather) {
        return std::nullopt;
    }

    QVariantList brush_points;
    const QVariantList candidate_points =
        source.value(QStringLiteral("brushPoints")).toList();
    constexpr int maximum_values = 3 * 4'096;
    if (candidate_points.size() > maximum_values || candidate_points.size() % 3 != 0) {
        return std::nullopt;
    }
    brush_points.reserve(candidate_points.size());
    for (qsizetype index = 0; index < candidate_points.size(); ++index) {
        bool converted = false;
        const double value = candidate_points.at(index).toDouble(&converted);
        const bool is_stroke_marker = index % 3 == 2;
        if (!converted || !std::isfinite(value)
            || (!is_stroke_marker && (value < 0.0 || value > 1.0))
            || (is_stroke_marker && value != 0.0 && value != 1.0)) {
            return std::nullopt;
        }
        brush_points.push_back(value);
    }

    if ((kind == 1 && std::hypot(*x1 - *x0, *y1 - *y0) <= std::numeric_limits<double>::epsilon())
        || (kind == 2 && (*radius_x <= 0.0 || *radius_y <= 0.0))
        || (kind == 3 && *radius_x <= 0.0)) {
        return std::nullopt;
    }

    return QVariantMap{
        {QStringLiteral("id"), id},
        {QStringLiteral("name"), name},
        {QStringLiteral("kind"), kind},
        {QStringLiteral("x0"), *x0},
        {QStringLiteral("y0"), *y0},
        {QStringLiteral("x1"), *x1},
        {QStringLiteral("y1"), *y1},
        {QStringLiteral("radiusX"), *radius_x},
        {QStringLiteral("radiusY"), *radius_y},
        {QStringLiteral("feather"), *feather},
        {QStringLiteral("inverted"), source.value(QStringLiteral("inverted")).toBool()},
        {QStringLiteral("brushPoints"), brush_points},
    };
}

[[nodiscard]] QVariantMap node_mask_asset_from_node(
    const QString& id,
    const QString& name,
    const BackendGradeNode& node
) {
    QVariantList brush_points;
    brush_points.reserve(node.local_mask_brush_points.size());
    for (const double value : node.local_mask_brush_points) {
        brush_points.push_back(value);
    }
    return {
        {QStringLiteral("id"), id},
        {QStringLiteral("name"), name},
        {QStringLiteral("kind"), static_cast<int>(node.local_mask_kind)},
        {QStringLiteral("x0"), node.local_mask_x0},
        {QStringLiteral("y0"), node.local_mask_y0},
        {QStringLiteral("x1"), node.local_mask_x1},
        {QStringLiteral("y1"), node.local_mask_y1},
        {QStringLiteral("radiusX"), node.local_mask_radius_x},
        {QStringLiteral("radiusY"), node.local_mask_radius_y},
        {QStringLiteral("feather"), node.local_mask_feather},
        {QStringLiteral("inverted"), node.local_mask_invert},
        {QStringLiteral("brushPoints"), brush_points},
    };
}

[[nodiscard]] BackendPointColorRange point_color_at(
    const BackendFineEditParameters& fine,
    const int index
) {
    if (index == 0 && point_color_count(fine) > 0) {
        return {
            .enabled = fine.color_range_enabled,
            .center_degrees = fine.color_range_center,
            .width_degrees = fine.color_range_width,
            .softness = fine.color_range_softness,
            .hue_shift_degrees = fine.color_range_hue,
            .saturation = fine.color_range_saturation,
            .lightness = fine.color_range_lightness,
        };
    }
    if (index > 0 && index - 1 < fine.additional_point_colors.size()) {
        return fine.additional_point_colors.at(index - 1);
    }
    throw std::out_of_range("invalid Point Color index");
}

void set_point_color_at(
    BackendFineEditParameters& fine,
    const int index,
    const BackendPointColorRange& range
) {
    if (index == 0) {
        fine.color_range_enabled = range.enabled;
        fine.color_range_center = range.center_degrees;
        fine.color_range_width = range.width_degrees;
        fine.color_range_softness = range.softness;
        fine.color_range_hue = range.hue_shift_degrees;
        fine.color_range_saturation = range.saturation;
        fine.color_range_lightness = range.lightness;
        return;
    }
    if (index > 0 && index - 1 < fine.additional_point_colors.size()) {
        fine.additional_point_colors[index - 1] = range;
        return;
    }
    throw std::out_of_range("invalid Point Color index");
}

[[nodiscard]] double srgb_to_linear(const double value) noexcept {
    return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
}

[[nodiscard]] double sampled_oklch_hue(const QColor& color) noexcept {
    const double red = srgb_to_linear(color.redF());
    const double green = srgb_to_linear(color.greenF());
    const double blue = srgb_to_linear(color.blueF());
    const double l = std::cbrt(0.4122214708 * red + 0.5363325363 * green + 0.0514459929 * blue);
    const double m = std::cbrt(0.2119034982 * red + 0.6806995451 * green + 0.1073969566 * blue);
    const double s = std::cbrt(0.0883024619 * red + 0.2817188376 * green + 0.6299787005 * blue);
    const double a = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s;
    const double b = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s;
    double hue = std::atan2(b, a) * 180.0 / std::numbers::pi;
    if (hue < 0.0) {
        hue += 360.0;
    }
    return hue;
}

[[nodiscard]] bool is_neutral_oklab_lightness_curve(
    const QVector<double>& points
) noexcept {
    return points.isEmpty()
        || (points.size() == 4 && points[0] == 0.0 && points[1] == 0.0
            && points[2] == 1.0 && points[3] == 1.0);
}

void ensure_oklab_lightness_curve(BackendFineEditParameters& fine) {
    if (fine.oklab_lightness_curve_points.isEmpty()) {
        fine.oklab_lightness_curve_points = {0.0, 0.0, 1.0, 1.0};
    }
}

void clear_neutral_oklab_lightness_curve(BackendFineEditParameters& fine) {
    if (is_neutral_oklab_lightness_curve(fine.oklab_lightness_curve_points)) {
        fine.oklab_lightness_curve_points.clear();
    }
}

[[nodiscard]] LocalizedUiMessage
edit_message(const char *const source,
             const std::initializer_list<LocalizedUiArgument> arguments = {}) {
  return {"EditController", source, arguments};
}

[[nodiscard]] bool bounded_profile_int(
    const QVariantMap& profile,
    const QString& key,
    const int minimum,
    const int maximum,
    int* const output
) {
    bool converted = false;
    const int value = profile.value(key).toInt(&converted);
    if (!converted || value < minimum || value > maximum) {
        return false;
    }
    *output = value;
    return true;
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

[[nodiscard]] QVector<ToneCurvePoint> tone_curve_model_points(
    const BackendGradeNode* const grade_node
) {
    if (grade_node == nullptr) {
        return {{0.0, 0.0}, {1.0, 1.0}};
    }
    const auto& source = grade_node->fine.oklab_lightness_curve_points;
    if (source.isEmpty() || source.size() % 2 != 0) {
        return {{0.0, 0.0}, {1.0, 1.0}};
    }
    QVector<ToneCurvePoint> points;
    points.reserve(source.size() / 2);
    for (qsizetype index = 0; index < source.size(); index += 2) {
        points.push_back({.x = source[index], .y = source[index + 1]});
    }
    return points;
}

[[nodiscard]] QVector<double> backend_oklab_lightness_curve_points(
    const ToneCurvePointModel& model
) {
    const auto source = model.points();
    QVector<double> points;
    points.reserve(source.size() * 2);
    for (const auto& point : source) {
        points.push_back(point.x);
        points.push_back(point.y);
    }
    return points;
}

[[nodiscard]] QString tone_curve_gesture_key(const int index) {
    return QStringLiteral("perceptual_tone_curve/point/%1").arg(index);
}

[[nodiscard]] QString display_grade_node_label(const QString& stored_label) {
    constexpr auto context = "EditController";
    const QString numbered_copy_marker = QStringLiteral(" Copy ");
    const qsizetype numbered_copy_index = stored_label.lastIndexOf(numbered_copy_marker);
    if (numbered_copy_index > 0) {
        const QString suffix = stored_label.mid(
            numbered_copy_index + numbered_copy_marker.size()
        );
        bool valid_number = false;
        const int number = suffix.toInt(&valid_number);
        if (valid_number && number >= 2 && QString::number(number) == suffix) {
            const QString base = stored_label.left(numbered_copy_index);
            return QCoreApplication::translate(
                       context,
                       QT_TRANSLATE_NOOP("EditController", "%1 Copy %2")
            ).arg(display_grade_node_label(base)).arg(number);
        }
    }
    const QString copy_suffix = QStringLiteral(" Copy");
    if (stored_label.endsWith(copy_suffix) && stored_label.size() > copy_suffix.size()) {
        const QString base = stored_label.left(stored_label.size() - copy_suffix.size());
        return QCoreApplication::translate(
                   context,
                   QT_TRANSLATE_NOOP("EditController", "%1 Copy")
        ).arg(display_grade_node_label(base));
    }
    if (stored_label.compare(QStringLiteral("Adjustments"), Qt::CaseInsensitive) == 0) {
        return QCoreApplication::translate(
            context,
            QT_TRANSLATE_NOOP("EditController", "Adjustments")
        );
    }
    const QString numbered_prefix = QStringLiteral("Adjustments ");
    if (stored_label.startsWith(numbered_prefix, Qt::CaseInsensitive)) {
        const QString suffix = stored_label.mid(numbered_prefix.size());
        bool valid_number = false;
        const int number = suffix.toInt(&valid_number);
        if (valid_number && number >= 2 && QString::number(number) == suffix) {
            return QCoreApplication::translate(
                       context,
                       QT_TRANSLATE_NOOP("EditController", "Adjustments %1")
            ).arg(number);
        }
    }
    return stored_label;
}

[[nodiscard]] bool grade_node_list_changed(
    const BackendGradeStack& before,
    const BackendGradeStack& after
) {
    if (before.grade_nodes.size() != after.grade_nodes.size()) {
        return true;
    }
    for (qsizetype index = 0; index < before.grade_nodes.size(); ++index) {
        const auto& left = before.grade_nodes.at(index);
        const auto& right = after.grade_nodes.at(index);
        if (left.grade_node_id != right.grade_node_id || left.label != right.label
            || left.enabled != right.enabled) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] QString history_grade_node_id(const std::string& key) {
    const QString value = QString::fromStdString(key);
    constexpr QLatin1StringView prefix("grade_node/");
    if (!value.startsWith(prefix)) {
        return {};
    }
    const qsizetype prefix_size = prefix.size();
    const qsizetype end = value.indexOf(QLatin1Char('/'), prefix_size);
    return end < 0
        ? value.mid(prefix_size)
        : value.mid(prefix_size, end - prefix_size);
}

} // namespace

EditController::EditController(
    std::shared_ptr<DesktopBackend> backend,
    std::shared_ptr<EditPreviewStore> preview_store,
    QObject* parent
)
    : QObject(parent),
      backend_(std::move(backend)),
      preview_store_(std::move(preview_store)),
      versions_(this),
      tone_curve_points_(this),
      node_mask_asset_settings_(std::make_unique<QSettings>()) {
    histogram_ = empty_histogram();
    before_histogram_ = empty_histogram();
    preview_debounce_.setSingleShot(true);
    detail_debounce_.setSingleShot(true);
    detail_warmup_debounce_.setSingleShot(true);
    autosave_debounce_.setSingleShot(true);
    connect(
        &preview_debounce_,
        &QTimer::timeout,
        this,
        &EditController::startPreviewRender
    );
    connect(
        &state_watcher_,
        &QFutureWatcher<EditStateTaskResult>::finished,
        this,
        &EditController::finishStateTask
    );
    connect(
        &preview_watcher_,
        &QFutureWatcher<EditPreviewTaskResult>::finished,
        this,
        &EditController::finishPreviewTask
    );
    connect(
        &detail_debounce_,
        &QTimer::timeout,
        this,
        &EditController::startDetailRender
    );
    connect(
        &autosave_debounce_,
        &QTimer::timeout,
        this,
        &EditController::startAutosave
    );
    connect(
        &detail_watcher_,
        &QFutureWatcher<EditDetailTaskResult>::finished,
        this,
        &EditController::finishDetailTask
    );
    connect(
        &detail_warmup_debounce_,
        &QTimer::timeout,
        this,
        &EditController::startDetailWarmup
    );
    connect(
        &detail_warmup_watcher_,
        &QFutureWatcher<EditDetailWarmupTaskResult>::finished,
        this,
        &EditController::finishDetailWarmupTask
    );
  if (auto *const application = QCoreApplication::instance()) {
    application->installEventFilter(this);
  }
  loadNodeMaskAssets();
  refreshSharedGradeNodes();
}

EditController::~EditController() {
    preview_debounce_.stop();
    detail_debounce_.stop();
    detail_warmup_debounce_.stop();
    autosave_debounce_.stop();
    detail_render_token_ = backend_->beginEditDetailRequest();
    detail_warmup_token_ = detail_render_token_;
    state_watcher_.waitForFinished();
    preview_watcher_.waitForFinished();
    detail_watcher_.waitForFinished();
    detail_warmup_watcher_.waitForFinished();
}

bool EditController::active() const noexcept {
    return active_;
}

bool EditController::busy() const noexcept {
    return state_running_ || current_rendering_ || before_rendering_ || detail_rendering_;
}

bool EditController::stateBusy() const noexcept {
    return interactionLocked();
}

bool EditController::interactionLocked() const noexcept {
    // Working snapshots are intentionally non-blocking: the editor keeps a
    // revisioned in-memory draft and rebases it on the committed autosave
    // head when the transaction returns. Opening a photo, creating a named
    // Version, and loading a Version still replace controller state, so they
    // remain interaction-locking operations.
    return state_running_ && state_task_kind_ != EditStateTaskKind::Autosave;
}

bool EditController::rendering() const noexcept {
    return current_rendering_;
}

bool EditController::beforeRendering() const noexcept {
    return before_rendering_;
}

bool EditController::detailMode() const noexcept {
    return detail_mode_;
}

bool EditController::detailRendering() const noexcept {
    return detail_rendering_;
}

QString EditController::detailErrorText() const {
    return detail_error_message_.translated();
}

quint32 EditController::detailFullWidth() const noexcept {
    return detail_full_width_;
}

quint32 EditController::detailFullHeight() const noexcept {
    return detail_full_height_;
}

quint64 EditController::detailRetainedBytes() const noexcept {
    return detail_retained_bytes_;
}

QVariantList EditController::detailTiles() const {
    return detail_tiles_;
}

bool EditController::fullResolutionPreparing() const noexcept {
    return full_resolution_preparing_;
}

bool EditController::fullResolutionReady() const noexcept {
    return full_resolution_ready_;
}

quint64 EditController::fullResolutionRetainedBytes() const noexcept {
    return full_resolution_retained_bytes_;
}

bool EditController::dirty() const noexcept {
    return dirty_;
}

bool EditController::autosavePending() const noexcept {
    return !autosaveFailed() && (autosave_requested_ || autosave_debounce_.isActive()
        || (state_running_ && state_task_kind_ == EditStateTaskKind::Autosave
            && state_watcher_.isRunning()));
}

bool EditController::autosaveFailed() const noexcept {
    return !autosave_error_message_.isEmpty();
}

QString EditController::autosaveErrorText() const {
    return autosave_error_message_.translated();
}

bool EditController::versionDraft() const noexcept {
    return version_draft_;
}

bool EditController::canUndo() const noexcept {
    return history_.canUndo();
}

bool EditController::canRedo() const noexcept {
    return history_.canRedo();
}

QString EditController::photoId() const {
    return photo_id_;
}

QString EditController::representationId() const {
    return representation_id_;
}

QString EditController::title() const {
    return title_;
}

QString EditController::sourcePath() const {
    return source_path_;
}

QString EditController::previewSource() const {
    return preview_source_;
}

QString EditController::provisionalPreviewSource() const {
    return provisional_preview_source_;
}

QString EditController::beforePreviewSource() const {
    return before_preview_source_;
}

QVariantMap EditController::histogram() const {
    return histogram_;
}

QVariantMap EditController::beforeHistogram() const {
    return before_histogram_;
}

QString EditController::beforeErrorText() const {
    return before_error_message_.translated();
}

bool EditController::recipeRecoveryRequired() const noexcept {
    return !recipe_recovery_message_.isEmpty();
}

QString EditController::recipeRecoveryErrorText() const {
    return recipe_recovery_message_.translated();
}

QString EditController::statusText() const {
    return status_message_.translated();
}

bool EditController::opticsEnabled() const noexcept { return grade_stack_.optics.enabled; }
bool EditController::opticsDistortionEnabled() const noexcept {
    return grade_stack_.optics.correct_distortion;
}
bool EditController::opticsTcaEnabled() const noexcept {
    return grade_stack_.optics.correct_tca;
}
bool EditController::opticsVignettingEnabled() const noexcept {
    return grade_stack_.optics.correct_vignetting;
}
bool EditController::opticsAutomaticScale() const noexcept {
    return grade_stack_.optics.automatic_scale;
}
int EditController::manualOpticsDistortion() const noexcept {
    return grade_stack_.optics.manual_distortion;
}
int EditController::manualOpticsTcaRedCyan() const noexcept {
    return grade_stack_.optics.manual_tca_red_cyan;
}
int EditController::manualOpticsTcaBlueYellow() const noexcept {
    return grade_stack_.optics.manual_tca_blue_yellow;
}
int EditController::manualOpticsVignettingAmount() const noexcept {
    return grade_stack_.optics.manual_vignetting_amount;
}
int EditController::manualOpticsVignettingMidpoint() const noexcept {
    return grade_stack_.optics.manual_vignetting_midpoint;
}
QVariantMap EditController::opticsReceipt() const { return optics_receipt_; }
bool EditController::opticsManualProfile() const noexcept {
    return !grade_stack_.optics.camera_profile_model.isEmpty()
        && !grade_stack_.optics.lens_profile_model.isEmpty();
}
QString EditController::opticsCameraProfile() const {
    return grade_stack_.optics.camera_profile_model;
}
QString EditController::opticsLensProfile() const {
    return grade_stack_.optics.lens_profile_model;
}

QVariantMap EditController::selectedLocalMask() const {
    const auto* const grade_node = selectedGradeNode();
    if (grade_node == nullptr) {
        return {{QStringLiteral("kind"), 0}};
    }
    QVariantList brush_points;
    brush_points.reserve(grade_node->local_mask_brush_points.size() / 3);
    for (qsizetype offset = 0;
         offset + 2 < grade_node->local_mask_brush_points.size();
         offset += 3) {
        brush_points.push_back(QVariantMap{
            {QStringLiteral("x"), grade_node->local_mask_brush_points.at(offset)},
            {QStringLiteral("y"), grade_node->local_mask_brush_points.at(offset + 1)},
            {
                QStringLiteral("beginsStroke"),
                grade_node->local_mask_brush_points.at(offset + 2) == 1.0
            },
        });
    }
    return {
        {QStringLiteral("kind"), static_cast<int>(grade_node->local_mask_kind)},
        {QStringLiteral("x0"), grade_node->local_mask_x0},
        {QStringLiteral("y0"), grade_node->local_mask_y0},
        {QStringLiteral("x1"), grade_node->local_mask_x1},
        {QStringLiteral("y1"), grade_node->local_mask_y1},
        {QStringLiteral("radiusX"), grade_node->local_mask_radius_x},
        {QStringLiteral("radiusY"), grade_node->local_mask_radius_y},
        {QStringLiteral("feather"), grade_node->local_mask_feather},
        {QStringLiteral("inverted"), grade_node->local_mask_invert},
        {QStringLiteral("brushPoints"), brush_points},
    };
}

bool EditController::hasCopiedNodeMask() const noexcept {
    return node_mask_clipboard_.has_value();
}

QVariantList EditController::nodeMaskAssets() const {
    return node_mask_assets_;
}

void EditController::loadNodeMaskAssets() {
    const QByteArray stored = node_mask_asset_settings_->value(
        QString::fromLatin1(NODE_MASK_ASSETS_SETTINGS_KEY)
    ).toByteArray();
    const QJsonDocument parsed = QJsonDocument::fromJson(stored);
    if (!parsed.isArray()) {
        return;
    }

    QSet<QString> ids;
    QSet<QString> names;
    const QVariantList stored_assets = parsed.toVariant().toList();
    for (const QVariant& value : stored_assets) {
        const auto asset = normalized_node_mask_asset(value.toMap());
        if (!asset.has_value() || node_mask_assets_.size() >= MAX_NODE_MASK_ASSET_COUNT) {
            continue;
        }
        const QString id = asset->value(QStringLiteral("id")).toString();
        const QString name = asset->value(QStringLiteral("name")).toString();
        const QString name_key = name.toCaseFolded();
        if (ids.contains(id) || names.contains(name_key)) {
            continue;
        }
        ids.insert(id);
        names.insert(name_key);
        node_mask_assets_.push_back(*asset);
    }
    std::sort(
        node_mask_assets_.begin(),
        node_mask_assets_.end(),
        [](const QVariant& left, const QVariant& right) {
            return left.toMap().value(QStringLiteral("name")).toString().compare(
                right.toMap().value(QStringLiteral("name")).toString(),
                Qt::CaseInsensitive
            ) < 0;
        }
    );
}

void EditController::persistNodeMaskAssets() {
    node_mask_asset_settings_->setValue(
        QString::fromLatin1(NODE_MASK_ASSETS_SETTINGS_KEY),
        QJsonDocument::fromVariant(node_mask_assets_).toJson(QJsonDocument::Compact)
    );
    node_mask_asset_settings_->sync();
}

QVariantList EditController::retouchSpots() const {
    QVariantList result;
    result.reserve(grade_stack_.retouch_spots.size());
    for (qsizetype index = 0; index < grade_stack_.retouch_spots.size(); ++index) {
        const auto& spot = grade_stack_.retouch_spots.at(index);
        result.push_back(QVariantMap{
            {QStringLiteral("index"), static_cast<int>(index)},
            {QStringLiteral("x"), spot.center_x},
            {QStringLiteral("y"), spot.center_y},
            {QStringLiteral("radius"), static_cast<int>(spot.radius_level_zero_pixels)},
            {QStringLiteral("mode"), static_cast<int>(spot.mode)},
            {QStringLiteral("sourceOffsetX"), spot.source_offset_x_radii},
            {QStringLiteral("sourceOffsetY"), spot.source_offset_y_radii},
            {QStringLiteral("feather"), spot.feather},
        });
    }
    return result;
}

QVariantList EditController::retouchStrokes() const {
    QVariantList result;
    result.reserve(grade_stack_.retouch_strokes.size());
    for (qsizetype index = 0; index < grade_stack_.retouch_strokes.size(); ++index) {
        const auto& stroke = grade_stack_.retouch_strokes.at(index);
        QVariantList points;
        points.reserve(stroke.points.size());
        for (const auto& point : stroke.points) {
            points.push_back(QVariantMap{
                {QStringLiteral("x"), point.x},
                {QStringLiteral("y"), point.y},
            });
        }
        result.push_back(QVariantMap{
            {QStringLiteral("index"), static_cast<int>(index)},
            {QStringLiteral("points"), points},
            {QStringLiteral("radius"), static_cast<int>(stroke.radius_level_zero_pixels)},
            {QStringLiteral("mode"), static_cast<int>(stroke.mode)},
            {QStringLiteral("sourceOffsetX"), stroke.source_offset_x_radii},
            {QStringLiteral("sourceOffsetY"), stroke.source_offset_y_radii},
            {QStringLiteral("feather"), stroke.feather},
        });
    }
    return result;
}

QVariantMap EditController::photoGeometry() const {
    const auto& geometry = grade_stack_.geometry;
    return {
        {QStringLiteral("cropLeft"), geometry.crop_left},
        {QStringLiteral("cropTop"), geometry.crop_top},
        {QStringLiteral("cropRight"), geometry.crop_right},
        {QStringLiteral("cropBottom"), geometry.crop_bottom},
        {QStringLiteral("quarterTurn"), static_cast<int>(geometry.quarter_turn)},
        {QStringLiteral("straightenDegrees"), geometry.straighten_degrees},
        {QStringLiteral("flipHorizontal"), geometry.flip_horizontal},
        {QStringLiteral("flipVertical"), geometry.flip_vertical},
        {QStringLiteral("identity"), geometry == BackendPhotoGeometry{}},
    };
}

bool EditController::cropToolActive() const noexcept {
    return crop_tool_active_;
}

QVariantList EditController::gradeNodes() const {
    QVariantList result;
    result.reserve(grade_stack_.grade_nodes.size());
    for (qsizetype index = 0; index < grade_stack_.grade_nodes.size(); ++index) {
        const auto& grade_node = grade_stack_.grade_nodes.at(index);
        QVariantMap item;
        item.insert(QStringLiteral("gradeNodeId"), grade_node.grade_node_id);
        item.insert(QStringLiteral("sharedLayerId"), grade_node.shared_layer_id);
        item.insert(QStringLiteral("sharedRevisionId"), grade_node.shared_revision_id);
        item.insert(
            QStringLiteral("shared"),
            !grade_node.shared_layer_id.isEmpty()
                && !grade_node.shared_revision_id.isEmpty()
        );
        const auto shared = std::find_if(
            shared_grade_nodes_.cbegin(),
            shared_grade_nodes_.cend(),
            [&grade_node](const BackendSharedGradeNode& candidate) {
                return candidate.layer_id == grade_node.shared_layer_id
                    && candidate.revision_id == grade_node.shared_revision_id;
            }
        );
        item.insert(
            QStringLiteral("sharedRevisionNumber"),
            shared == shared_grade_nodes_.cend()
                ? 0
                : static_cast<int>(shared->revision_number)
        );
        item.insert(
            QStringLiteral("label"),
            display_grade_node_label(grade_node.label)
        );
        item.insert(QStringLiteral("rawLabel"), grade_node.label);
        item.insert(QStringLiteral("enabled"), grade_node.enabled);
        item.insert(
            QStringLiteral("hasLocalMask"),
            grade_node.local_mask_kind != 0U
        );
        item.insert(QStringLiteral("index"), static_cast<int>(index));
        result.push_back(item);
    }
    return result;
}

QVariantList EditController::sharedGradeNodes() const {
    QVariantList result;
    result.reserve(shared_grade_nodes_.size());
    for (const auto& shared : shared_grade_nodes_) {
        result.push_back(QVariantMap{
            {QStringLiteral("layerId"), shared.layer_id},
            {QStringLiteral("revisionId"), shared.revision_id},
            {QStringLiteral("revisionNumber"), shared.revision_number},
            {QStringLiteral("label"), shared.label},
        });
    }
    return result;
}

int EditController::selectedGradeNodeIndex() const noexcept {
    return selected_grade_node_index_;
}

QString EditController::selectedGradeNodeId() const {
    const auto* const grade_node = selectedGradeNode();
    return grade_node == nullptr ? QString{} : grade_node->grade_node_id;
}

bool EditController::hasSelectedGradeNode() const noexcept {
    return selectedGradeNode() != nullptr;
}

bool EditController::canAddGradeNode() const noexcept {
    return active_ && !interactionLocked()
        && grade_stack_.grade_nodes.size() < GradeNodeStack::maximum_grade_node_count;
}

bool EditController::canDeleteGradeNode() const noexcept {
    return active_ && !interactionLocked() && hasSelectedGradeNode()
        && grade_stack_.grade_nodes.size() > GradeNodeStack::minimum_grade_node_count;
}

bool EditController::canMoveGradeNodeUp() const noexcept {
    return active_ && !interactionLocked() && selected_grade_node_index_ > 0;
}

bool EditController::canMoveGradeNodeDown() const noexcept {
    const int count = static_cast<int>(grade_stack_.grade_nodes.size());
    return active_ && !interactionLocked() && selected_grade_node_index_ >= 0
        && selected_grade_node_index_ + 1 < count;
}

bool EditController::gradeNodeEnabled() const noexcept {
    const auto* const grade_node = selectedGradeNode();
    return grade_node != nullptr && grade_node->enabled;
}

double EditController::exposureStops() const noexcept {
    const auto* const grade_node = selectedGradeNode();
    return grade_node == nullptr ? 0.0 : grade_node->basic.exposure_stops;
}

double EditController::contrastFactor() const noexcept {
    const auto* const grade_node = selectedGradeNode();
    return grade_node == nullptr ? 1.0 : grade_node->basic.contrast_factor;
}

double EditController::whiteBalanceTemperature() const noexcept {
    const auto* const grade_node = selectedGradeNode();
    return grade_node == nullptr ? 0.0 : grade_node->basic.white_balance_temperature;
}

double EditController::whiteBalanceTint() const noexcept {
    const auto* const grade_node = selectedGradeNode();
    return grade_node == nullptr ? 0.0 : grade_node->basic.white_balance_tint;
}

double EditController::saturationFactor() const noexcept {
    const auto* const grade_node = selectedGradeNode();
    return grade_node == nullptr ? 1.0 : grade_node->basic.saturation_factor;
}

QString EditController::lutResourceId() const {
    const auto* const grade_node = selectedGradeNode();
    return grade_node == nullptr ? QString{} : grade_node->fine.lut_resource_id;
}

QString EditController::lutTitle() const {
    const auto* const grade_node = selectedGradeNode();
    return grade_node == nullptr ? QString{} : grade_node->fine.lut_title;
}

bool EditController::hasLut() const noexcept {
    const auto* const grade_node = selectedGradeNode();
    return grade_node != nullptr && !grade_node->fine.lut_resource_id.isEmpty();
}

double EditController::lutIntensity() const noexcept {
    const auto* const grade_node = selectedGradeNode();
    return grade_node == nullptr ? 1.0 : grade_node->fine.lut_intensity;
}

quint64 EditController::parameterRevision() const noexcept {
    return parameter_revision_;
}

QVariantList EditController::pointColors() const {
    QVariantList result;
    const auto* const grade_node = selectedGradeNode();
    if (grade_node == nullptr) {
        return result;
    }
    const int count = point_color_count(grade_node->fine);
    result.reserve(count);
    for (int index = 0; index < count; ++index) {
        const auto range = point_color_at(grade_node->fine, index);
        const QColor swatch = QColor::fromHslF(
            static_cast<float>(std::fmod(range.center_degrees + 360.0, 360.0) / 360.0),
            0.72F,
            0.55F
        );
        result.push_back(QVariantMap{
            {QStringLiteral("index"), index},
            {QStringLiteral("enabled"), range.enabled},
            {QStringLiteral("hue"), range.center_degrees},
            {QStringLiteral("swatch"), swatch.name(QColor::HexRgb)},
        });
    }
    return result;
}

QVariantList EditController::colorWarperControlPoints() const {
    QVariantList result;
    const auto* const grade_node = selectedGradeNode();
    const BackendFineEditParameters neutral;
    const auto& points = grade_node == nullptr
        ? neutral.oklab_color_warper_control_points
        : grade_node->fine.oklab_color_warper_control_points;
    result.reserve(static_cast<qsizetype>(points.size()));
    for (std::size_t index = 0U; index < points.size(); ++index) {
        const auto& point = points[index];
        result.push_back(QVariantMap{
            {QStringLiteral("index"), static_cast<int>(index)},
            {QStringLiteral("row"), static_cast<int>(
                index / BACKEND_OKLAB_COLOR_WARPER_GRID_SIDE)},
            {QStringLiteral("column"), static_cast<int>(
                index % BACKEND_OKLAB_COLOR_WARPER_GRID_SIDE)},
            {QStringLiteral("aOffset"), point.a_offset},
            {QStringLiteral("bOffset"), point.b_offset},
        });
    }
    return result;
}

int EditController::selectedPointColorIndex() const noexcept {
    return selected_point_color_index_;
}

std::optional<PreviewScopeHueQualifier> EditController::selectedPointColorScopeQualifier() const {
    const auto* const grade_node = selectedGradeNode();
    if (grade_node == nullptr || selected_point_color_index_ < 0
        || selected_point_color_index_ >= point_color_count(grade_node->fine)) {
        return std::nullopt;
    }
    const BackendPointColorRange range = point_color_at(
        grade_node->fine,
        selected_point_color_index_
    );
    if (!range.enabled || !std::isfinite(range.center_degrees)
        || range.center_degrees < 0.0 || range.center_degrees > 360.0
        || !std::isfinite(range.width_degrees) || range.width_degrees < 1.0
        || range.width_degrees > 180.0 || !std::isfinite(range.softness)
        || range.softness < 0.0 || range.softness > 1.0) {
        return std::nullopt;
    }
    return PreviewScopeHueQualifier{
        .center_degrees = range.center_degrees,
        .width_degrees = range.width_degrees,
        .softness = range.softness,
    };
}

bool EditController::pointColorScopeActive() const noexcept {
    return point_color_scope_active_;
}

bool EditController::pointColorScopeAvailable() const noexcept {
    return selectedPointColorScopeQualifier().has_value();
}

bool EditController::pointColorPickerActive() const noexcept {
    return point_color_picker_active_;
}

bool EditController::retouchPickerActive() const noexcept {
    return retouch_picker_active_;
}

int EditController::retouchCreationMode() const noexcept {
    return retouch_creation_mode_;
}

bool EditController::whiteBalancePickerActive() const noexcept {
    return white_balance_picker_active_;
}

double EditController::parameterValue(const QString& parameter_key) const {
    const auto* const grade_node = selectedGradeNode();
    const BackendFineEditParameters neutral;
    const BackendFineEditParameters& fine = grade_node == nullptr
        ? neutral
        : grade_node->fine;
    if (parameter_key == QStringLiteral("highlights")) {
        return fine.highlights;
    }
    if (parameter_key == QStringLiteral("shadows")) {
        return fine.shadows;
    }
    if (parameter_key == QStringLiteral("whites")) {
        return fine.whites;
    }
    if (parameter_key == QStringLiteral("blacks")) {
        return fine.blacks;
    }
    if (parameter_key == QStringLiteral("global_a_balance")) {
        return fine.global_a_balance;
    }
    if (parameter_key == QStringLiteral("global_b_balance")) {
        return fine.global_b_balance;
    }
    if (parameter_key == QStringLiteral("vibrance")) {
        return fine.vibrance;
    }
    if (parameter_key == QStringLiteral("color_warper_strength")) {
        return fine.oklab_color_warper_strength;
    }
    if (parameter_key == QStringLiteral("lut_intensity")) {
        return fine.lut_intensity;
    }
    if (parameter_key == QStringLiteral("sharpen_amount")) {
        return fine.sharpen_amount;
    }
    if (parameter_key == QStringLiteral("sharpen_radius")) {
        return fine.sharpen_radius;
    }
    if (parameter_key == QStringLiteral("sharpen_threshold")) {
        return fine.sharpen_threshold;
    }
    if (parameter_key == QStringLiteral("sharpen_masking")) {
        return fine.sharpen_masking;
    }
    if (parameter_key == QStringLiteral("clarity")) return fine.clarity;
    if (parameter_key == QStringLiteral("texture")) return fine.texture;
    if (parameter_key == QStringLiteral("local_contrast")) return fine.local_contrast;
    if (parameter_key == QStringLiteral("local_contrast_scale")) {
        return fine.local_contrast_scale;
    }
    if (parameter_key == QStringLiteral("selective_color_lightness_protection")) {
        return fine.selective_color_lightness_protection;
    }
    if (parameter_key == QStringLiteral("denoise_luminance")) return fine.denoise_luminance;
    if (parameter_key == QStringLiteral("denoise_detail")) return fine.denoise_detail;
    if (parameter_key == QStringLiteral("denoise_color")) return fine.denoise_color;
    if (parameter_key == QStringLiteral("dehaze")) return fine.dehaze;
    if (parameter_key == QStringLiteral("defringe_purple_amount")) {
        return fine.defringe_purple_amount;
    }
    if (parameter_key == QStringLiteral("defringe_purple_hue_low")) {
        return fine.defringe_purple_hue_low;
    }
    if (parameter_key == QStringLiteral("defringe_purple_hue_high")) {
        return fine.defringe_purple_hue_high;
    }
    if (parameter_key == QStringLiteral("defringe_green_amount")) {
        return fine.defringe_green_amount;
    }
    if (parameter_key == QStringLiteral("defringe_green_hue_low")) {
        return fine.defringe_green_hue_low;
    }
    if (parameter_key == QStringLiteral("defringe_green_hue_high")) {
        return fine.defringe_green_hue_high;
    }
    if (parameter_key == QStringLiteral("shadows_hue")) return fine.shadows_hue;
    if (parameter_key == QStringLiteral("shadows_saturation")) return fine.shadows_saturation;
    if (parameter_key == QStringLiteral("shadows_luminance")) return fine.shadows_luminance;
    if (parameter_key == QStringLiteral("midtones_hue")) return fine.midtones_hue;
    if (parameter_key == QStringLiteral("midtones_saturation")) return fine.midtones_saturation;
    if (parameter_key == QStringLiteral("midtones_luminance")) return fine.midtones_luminance;
    if (parameter_key == QStringLiteral("highlights_hue")) return fine.highlights_hue;
    if (parameter_key == QStringLiteral("highlights_saturation")) return fine.highlights_saturation;
    if (parameter_key == QStringLiteral("highlights_luminance")) return fine.highlights_luminance;
    if (parameter_key == QStringLiteral("grading_blending")) return fine.grading_blending;
    if (parameter_key == QStringLiteral("grading_balance")) return fine.grading_balance;
    if (parameter_key == QStringLiteral("grain_amount")) return fine.grain_amount;
    if (parameter_key == QStringLiteral("grain_size")) return fine.grain_size;
    if (parameter_key == QStringLiteral("grain_roughness")) return fine.grain_roughness;
    if (parameter_key == QStringLiteral("vignette_amount")) return fine.vignette_amount;
    if (parameter_key == QStringLiteral("vignette_midpoint")) return fine.vignette_midpoint;
    if (parameter_key == QStringLiteral("vignette_roundness")) return fine.vignette_roundness;
    if (parameter_key == QStringLiteral("vignette_feather")) return fine.vignette_feather;
    if (parameter_key == QStringLiteral("vignette_highlights")) return fine.vignette_highlights;
    if (parameter_key.startsWith(QStringLiteral("color_range_"))) {
        if (selected_point_color_index_ < 0
            || selected_point_color_index_ >= point_color_count(fine)) {
            return parameter_key == QStringLiteral("color_range_width") ? 30.0
                : parameter_key == QStringLiteral("color_range_softness") ? 0.5 : 0.0;
        }
        const auto range = point_color_at(fine, selected_point_color_index_);
        if (parameter_key == QStringLiteral("color_range_enabled")) return range.enabled ? 1.0 : 0.0;
        if (parameter_key == QStringLiteral("color_range_center")) return range.center_degrees;
        if (parameter_key == QStringLiteral("color_range_width")) return range.width_degrees;
        if (parameter_key == QStringLiteral("color_range_softness")) return range.softness;
        if (parameter_key == QStringLiteral("color_range_hue")) return range.hue_shift_degrees;
        if (parameter_key == QStringLiteral("color_range_saturation")) return range.saturation;
        if (parameter_key == QStringLiteral("color_range_lightness")) return range.lightness;
    }
    return 0.0;
}

double EditController::colorMixerValue(
    const int band_index,
    const QString& component
) const {
    const auto* const grade_node = selectedGradeNode();
    if (grade_node == nullptr || band_index < 0
        || static_cast<std::size_t>(band_index) >= BACKEND_COLOR_MIXER_BAND_COUNT) {
        return 0.0;
    }
    const std::size_t index = static_cast<std::size_t>(band_index);
    if (component == QStringLiteral("hue")) {
        return grade_node->fine.mixer_hue[index];
    }
    if (component == QStringLiteral("saturation")) {
        return grade_node->fine.mixer_saturation[index];
    }
    if (component == QStringLiteral("lightness")) {
        return grade_node->fine.mixer_lightness[index];
    }
    return 0.0;
}

QAbstractItemModel* EditController::toneCurvePoints() noexcept {
    return &tone_curve_points_;
}

bool EditController::hasToneCurve() const noexcept {
    const auto* const grade_node = selectedGradeNode();
    return grade_node != nullptr
        && !is_neutral_oklab_lightness_curve(grade_node->fine.oklab_lightness_curve_points);
}

bool EditController::toneCurveEditable() const noexcept {
    return tone_curve_points_.isEditable();
}

QAbstractItemModel* EditController::versions() noexcept {
    return &versions_;
}

void EditController::setGradeNodeEnabled(const bool enabled) {
    const auto* const grade_node = selectedGradeNode();
    if (!active_ || interactionLocked() || grade_node == nullptr
        || grade_node->enabled == enabled) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    const QString grade_node_id = grade_node->grade_node_id;
    grade_stack_.grade_nodes[selected_grade_node_index_].enabled = enabled;
    recordWorkingTransition(
        QStringLiteral("grade_node/%1/enabled").arg(grade_node_id),
        before
    );
    emit gradeNodesChanged();
    emit gradeNodeEnabledChanged();
    setDirty(version_draft_ || grade_stack_ != committed_grade_stack_);
    schedulePreview(0);
  setStatusMessage(edit_message(
        enabled ? QT_TRANSLATE_NOOP("EditController", "Grade Node enabled")
                : QT_TRANSLATE_NOOP(
                    "EditController",
                    "Grade Node bypassed · settings preserved"))
    );
}

void EditController::setExposureStops(const double value) {
    const auto* const grade_node = selectedGradeNode();
    if (grade_node == nullptr || grade_node->basic.exposure_stops == value
        || !acceptParameter(value, -16.0, 16.0,
                       QT_TRANSLATE_NOOP("EditController", "Exposure"))) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    grade_stack_.grade_nodes[selected_grade_node_index_].basic.exposure_stops = value;
    parameterEdited(QStringLiteral("exposure"), before);
}

void EditController::setContrastFactor(const double value) {
    const auto* const grade_node = selectedGradeNode();
    if (grade_node == nullptr || grade_node->basic.contrast_factor == value
        || !acceptParameter(value, 0.0, 8.0,
                       QT_TRANSLATE_NOOP("EditController", "Contrast"))) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    grade_stack_.grade_nodes[selected_grade_node_index_].basic.contrast_factor = value;
    parameterEdited(QStringLiteral("contrast"), before);
}

void EditController::setWhiteBalanceTemperature(const double value) {
    const auto* const grade_node = selectedGradeNode();
    if (grade_node == nullptr || grade_node->basic.white_balance_temperature == value
        || !acceptParameter(value, -1.0, 1.0,
                       QT_TRANSLATE_NOOP("EditController", "Temperature"))) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    grade_stack_.grade_nodes[selected_grade_node_index_].basic.white_balance_temperature = value;
    parameterEdited(QStringLiteral("white_balance_temperature"), before);
}

void EditController::setWhiteBalanceTint(const double value) {
    const auto* const grade_node = selectedGradeNode();
    if (grade_node == nullptr || grade_node->basic.white_balance_tint == value
        || !acceptParameter(value, -1.0, 1.0,
                       QT_TRANSLATE_NOOP("EditController", "Tint"))) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    grade_stack_.grade_nodes[selected_grade_node_index_].basic.white_balance_tint = value;
    parameterEdited(QStringLiteral("white_balance_tint"), before);
}

void EditController::setSaturationFactor(const double value) {
    const auto* const grade_node = selectedGradeNode();
    if (grade_node == nullptr || grade_node->basic.saturation_factor == value
        || !acceptParameter(value, 0.0, 8.0,
                       QT_TRANSLATE_NOOP("EditController", "Chroma"))) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    grade_stack_.grade_nodes[selected_grade_node_index_].basic.saturation_factor = value;
    parameterEdited(QStringLiteral("saturation"), before);
}

void EditController::setLutIntensity(const double value) {
    setParameterValue(QStringLiteral("lut_intensity"), value);
}

void EditController::setOpticsEnabled(const bool enabled) {
    // A Lensfun profile is one coherent correction, not four unrelated
    // user-facing filters.  When a profile is enabled, ask it for every
    // calibrated correction it can supply; unavailable records remain a
    // harmless no-op and are reported through the receipt.  The individual
    // fields stay in the recipe ABI for now so v1 readers remain stable, but
    // they are no longer an editing choice in the desktop product.
    const bool profile_already_complete = !enabled
        || (grade_stack_.optics.correct_distortion
            && grade_stack_.optics.correct_tca
            && grade_stack_.optics.correct_vignetting
            && grade_stack_.optics.automatic_scale);
    if (!active_ || interactionLocked()
        || (grade_stack_.optics.enabled == enabled && profile_already_complete)) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    grade_stack_.optics.enabled = enabled;
    if (enabled) {
        grade_stack_.optics.correct_distortion = true;
        grade_stack_.optics.correct_tca = true;
        grade_stack_.optics.correct_vignetting = true;
        grade_stack_.optics.automatic_scale = true;
    }
    opticsEdited(QStringLiteral("profile"), before);
}

void EditController::setOpticsDistortionEnabled(const bool enabled) {
    if (!active_ || interactionLocked() || grade_stack_.optics.correct_distortion == enabled) return;
    const BackendGradeStack before = grade_stack_;
    grade_stack_.optics.correct_distortion = enabled;
    opticsEdited(QStringLiteral("distortion"), before);
}

void EditController::setOpticsTcaEnabled(const bool enabled) {
    if (!active_ || interactionLocked() || grade_stack_.optics.correct_tca == enabled) return;
    const BackendGradeStack before = grade_stack_;
    grade_stack_.optics.correct_tca = enabled;
    opticsEdited(QStringLiteral("tca"), before);
}

void EditController::setOpticsVignettingEnabled(const bool enabled) {
    if (!active_ || interactionLocked() || grade_stack_.optics.correct_vignetting == enabled) return;
    const BackendGradeStack before = grade_stack_;
    grade_stack_.optics.correct_vignetting = enabled;
    opticsEdited(QStringLiteral("vignetting"), before);
}

void EditController::setOpticsAutomaticScale(const bool enabled) {
    if (!active_ || interactionLocked() || grade_stack_.optics.automatic_scale == enabled) return;
    const BackendGradeStack before = grade_stack_;
    grade_stack_.optics.automatic_scale = enabled;
    opticsEdited(QStringLiteral("automatic_scale"), before);
}

void EditController::setManualOpticsDistortion(const int value) {
    if (!active_ || interactionLocked() || value < -100 || value > 100
        || grade_stack_.optics.manual_distortion == value) return;
    const BackendGradeStack before = grade_stack_;
    grade_stack_.optics.manual_distortion = static_cast<std::int16_t>(value);
    opticsEdited(QStringLiteral("manual_distortion"), before);
}

void EditController::setManualOpticsTcaRedCyan(const int value) {
    if (!active_ || interactionLocked() || value < -100 || value > 100
        || grade_stack_.optics.manual_tca_red_cyan == value) return;
    const BackendGradeStack before = grade_stack_;
    grade_stack_.optics.manual_tca_red_cyan = static_cast<std::int16_t>(value);
    opticsEdited(QStringLiteral("manual_tca_red_cyan"), before);
}

void EditController::setManualOpticsTcaBlueYellow(const int value) {
    if (!active_ || interactionLocked() || value < -100 || value > 100
        || grade_stack_.optics.manual_tca_blue_yellow == value) return;
    const BackendGradeStack before = grade_stack_;
    grade_stack_.optics.manual_tca_blue_yellow = static_cast<std::int16_t>(value);
    opticsEdited(QStringLiteral("manual_tca_blue_yellow"), before);
}

void EditController::setManualOpticsVignettingAmount(const int value) {
    if (!active_ || interactionLocked() || value < -100 || value > 100
        || grade_stack_.optics.manual_vignetting_amount == value) return;
    const BackendGradeStack before = grade_stack_;
    grade_stack_.optics.manual_vignetting_amount = static_cast<std::int16_t>(value);
    opticsEdited(QStringLiteral("manual_vignetting_amount"), before);
}

void EditController::setManualOpticsVignettingMidpoint(const int value) {
    if (!active_ || interactionLocked() || value < 0 || value > 100
        || grade_stack_.optics.manual_vignetting_midpoint == value) return;
    const BackendGradeStack before = grade_stack_;
    grade_stack_.optics.manual_vignetting_midpoint = static_cast<std::uint8_t>(value);
    opticsEdited(QStringLiteral("manual_vignetting_midpoint"), before);
}

QVariantList EditController::opticsProfileCandidates() {
    if (!active_ || photo_id_.isEmpty() || source_path_.isEmpty()) return {};
    try {
        return backend_->opticsProfileCandidates(photo_id_, source_path_);
    } catch (const std::exception& error) {
        setStatusMessage(edit_message(
            QT_TRANSLATE_NOOP("EditController", "Could not read Lensfun profiles · %1"),
            {QString::fromUtf8(error.what())}
        ));
        return {};
    }
}

void EditController::applyManualOpticsProfile(const QVariantMap& profile) {
    if (!active_ || interactionLocked()) {
        return;
    }
    int distortion = 0;
    int tca_red_cyan = 0;
    int tca_blue_yellow = 0;
    int vignetting_amount = 0;
    int vignetting_midpoint = 50;
    const bool valid = bounded_profile_int(
                           profile,
                           QStringLiteral("manualDistortion"),
                           -100,
                           100,
                           &distortion
                       )
        && bounded_profile_int(
            profile,
            QStringLiteral("manualTcaRedCyan"),
            -100,
            100,
            &tca_red_cyan
        )
        && bounded_profile_int(
            profile,
            QStringLiteral("manualTcaBlueYellow"),
            -100,
            100,
            &tca_blue_yellow
        )
        && bounded_profile_int(
            profile,
            QStringLiteral("manualVignettingAmount"),
            -100,
            100,
            &vignetting_amount
        )
        && bounded_profile_int(
            profile,
            QStringLiteral("manualVignettingMidpoint"),
            0,
            100,
            &vignetting_midpoint
        );
    if (!valid) {
        setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
            "EditController", "This local optical profile is invalid")));
        return;
    }
    if (grade_stack_.optics.manual_distortion == distortion
        && grade_stack_.optics.manual_tca_red_cyan == tca_red_cyan
        && grade_stack_.optics.manual_tca_blue_yellow == tca_blue_yellow
        && grade_stack_.optics.manual_vignetting_amount == vignetting_amount
        && grade_stack_.optics.manual_vignetting_midpoint == vignetting_midpoint) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    grade_stack_.optics.manual_distortion = static_cast<std::int16_t>(distortion);
    grade_stack_.optics.manual_tca_red_cyan = static_cast<std::int16_t>(tca_red_cyan);
    grade_stack_.optics.manual_tca_blue_yellow = static_cast<std::int16_t>(tca_blue_yellow);
    grade_stack_.optics.manual_vignetting_amount = static_cast<std::int16_t>(vignetting_amount);
    grade_stack_.optics.manual_vignetting_midpoint = static_cast<std::uint8_t>(vignetting_midpoint);
    const QString profile_id = profile.value(QStringLiteral("id")).toString();
    opticsEdited(
        QStringLiteral("manual_profile/%1").arg(profile_id.isEmpty()
            ? QStringLiteral("custom") : profile_id),
        before
    );
    const QString title = profile.value(QStringLiteral("title")).toString().trimmed();
    setStatusMessage(edit_message(
        QT_TRANSLATE_NOOP("EditController", "Applied manual optical profile · %1"),
        {title.isEmpty() ? tr("Custom profile") : title}
    ));
}

void EditController::setManualOpticsProfile(
    const QString& camera_maker,
    const QString& camera_model,
    const QString& lens_maker,
    const QString& lens_model
) {
    if (!active_ || interactionLocked() || camera_model.trimmed().isEmpty()
        || lens_model.trimmed().isEmpty()) return;
    const BackendGradeStack before = grade_stack_;
    // A user-selected profile follows the same all-calibrated-corrections
    // policy as automatic matching.  Manual controls below the profile tab
    // are deliberately additive residual corrections instead.
    grade_stack_.optics.enabled = true;
    grade_stack_.optics.correct_distortion = true;
    grade_stack_.optics.correct_tca = true;
    grade_stack_.optics.correct_vignetting = true;
    grade_stack_.optics.automatic_scale = true;
    grade_stack_.optics.camera_profile_maker = camera_maker.trimmed();
    grade_stack_.optics.camera_profile_model = camera_model.trimmed();
    grade_stack_.optics.lens_profile_maker = lens_maker.trimmed();
    grade_stack_.optics.lens_profile_model = lens_model.trimmed();
    opticsEdited(QStringLiteral("profile"), before);
}

void EditController::clearManualOpticsProfile() {
    if (!active_ || interactionLocked() || !opticsManualProfile()) return;
    const BackendGradeStack before = grade_stack_;
    grade_stack_.optics.camera_profile_maker.clear();
    grade_stack_.optics.camera_profile_model.clear();
    grade_stack_.optics.lens_profile_maker.clear();
    grade_stack_.optics.lens_profile_model.clear();
    opticsEdited(QStringLiteral("profile"), before);
}

void EditController::setLutResource(
    const QString& resource_id,
    const QString& title,
    const QString& managed_path
) {
    if (!active_ || interactionLocked()) {
        return;
    }
    auto* const grade_node = selected_grade_node_index_ < 0
        ? nullptr : &grade_stack_.grade_nodes[selected_grade_node_index_];
    const QFileInfo path(managed_path);
    const bool valid_id = resource_id.size() == 64
        && std::ranges::all_of(resource_id, [](const QChar character) {
            return (character >= QLatin1Char('0') && character <= QLatin1Char('9'))
                || (character >= QLatin1Char('a') && character <= QLatin1Char('f'));
        });
    if (grade_node == nullptr || !valid_id || title.trimmed().isEmpty()
        || title.toUtf8().size() > 512 || !path.isAbsolute()
        || path.suffix() != QStringLiteral("cube")
        || path.completeBaseName() != resource_id) {
        return;
    }
    auto& fine = grade_node->fine;
    if (fine.lut_resource_id == resource_id && fine.lut_title == title
        && fine.lut_managed_path == managed_path) {
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    fine.lut_resource_id = resource_id;
    fine.lut_title = title;
    fine.lut_managed_path = managed_path;
    parameterEdited(QStringLiteral("lut/resource"), before);
}

void EditController::clearLut() {
    if (!active_ || interactionLocked()) {
        return;
    }
    auto* const grade_node = selected_grade_node_index_ < 0
        ? nullptr : &grade_stack_.grade_nodes[selected_grade_node_index_];
    if (grade_node == nullptr || grade_node->fine.lut_resource_id.isEmpty()) {
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    grade_node->fine.lut_resource_id.clear();
    grade_node->fine.lut_title.clear();
    grade_node->fine.lut_managed_path.clear();
    parameterEdited(QStringLiteral("lut/resource"), before);
}

void EditController::setColorGradingWheel(
    const QString& tonal_range,
    const double hue,
    const double saturation
) {
    const auto* const selected = selectedGradeNode();
    if (selected == nullptr
        || !acceptParameter(hue, 0.0, 360.0,
                            QT_TRANSLATE_NOOP("EditController", "Color grading hue"))
        || !acceptParameter(saturation, 0.0, 1.0,
                            QT_TRANSLATE_NOOP("EditController", "Color grading saturation"))) {
        return;
    }
    auto& fine = grade_stack_.grade_nodes[selected_grade_node_index_].fine;
    double* target_hue = nullptr;
    double* target_saturation = nullptr;
    if (tonal_range == QStringLiteral("shadows")) {
        target_hue = &fine.shadows_hue;
        target_saturation = &fine.shadows_saturation;
    } else if (tonal_range == QStringLiteral("midtones")) {
        target_hue = &fine.midtones_hue;
        target_saturation = &fine.midtones_saturation;
    } else if (tonal_range == QStringLiteral("highlights")) {
        target_hue = &fine.highlights_hue;
        target_saturation = &fine.highlights_saturation;
    } else {
        return;
    }
    if (*target_hue == hue && *target_saturation == saturation) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    *target_hue = hue;
    *target_saturation = saturation;
    parameterEdited(QStringLiteral("color_grading/%1/wheel").arg(tonal_range), before);
}

void EditController::setDefringeHueRange(
    const QString& family,
    const double lower_hue,
    const double upper_hue
) {
    const auto* const selected = selectedGradeNode();
    if (selected == nullptr || upper_hue - lower_hue < 10.0
        || !acceptParameter(lower_hue, 0.0, 360.0,
                            QT_TRANSLATE_NOOP("EditController", "Defringe hue range"))
        || !acceptParameter(upper_hue, 0.0, 360.0,
                            QT_TRANSLATE_NOOP("EditController", "Defringe hue range"))) {
        return;
    }
    auto& fine = grade_stack_.grade_nodes[selected_grade_node_index_].fine;
    double* target_low = nullptr;
    double* target_high = nullptr;
    if (family == QStringLiteral("purple")) {
        target_low = &fine.defringe_purple_hue_low;
        target_high = &fine.defringe_purple_hue_high;
    } else if (family == QStringLiteral("green")) {
        target_low = &fine.defringe_green_hue_low;
        target_high = &fine.defringe_green_hue_high;
    } else {
        return;
    }
    if (*target_low == lower_hue && *target_high == upper_hue) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    *target_low = lower_hue;
    *target_high = upper_hue;
    parameterEdited(QStringLiteral("optics/defringe/%1/hue_range").arg(family), before);
}

void EditController::setParameterValue(
    const QString& parameter_key,
    const double value
) {
    const auto* const selected = selectedGradeNode();
    if (selected == nullptr) {
        return;
    }
    auto& fine = grade_stack_.grade_nodes[selected_grade_node_index_].fine;
    if (parameter_key.startsWith(QStringLiteral("color_range_"))) {
        const int count = point_color_count(fine);
        if (selected_point_color_index_ < 0 || selected_point_color_index_ >= count) {
            return;
        }
        BackendPointColorRange range = point_color_at(fine, selected_point_color_index_);
        double minimum = -1.0;
        double maximum = 1.0;
        double current = 0.0;
        if (parameter_key == QStringLiteral("color_range_enabled")) {
            minimum = 0.0; maximum = 1.0; current = range.enabled ? 1.0 : 0.0;
        } else if (parameter_key == QStringLiteral("color_range_center")) {
            minimum = 0.0; maximum = 360.0; current = range.center_degrees;
        } else if (parameter_key == QStringLiteral("color_range_width")) {
            minimum = 1.0; maximum = 180.0; current = range.width_degrees;
        } else if (parameter_key == QStringLiteral("color_range_softness")) {
            minimum = 0.0; maximum = 1.0; current = range.softness;
        } else if (parameter_key == QStringLiteral("color_range_hue")) {
            minimum = -180.0; maximum = 180.0; current = range.hue_shift_degrees;
        } else if (parameter_key == QStringLiteral("color_range_saturation")) {
            current = range.saturation;
        } else if (parameter_key == QStringLiteral("color_range_lightness")) {
            current = range.lightness;
        } else {
            return;
        }
        if (current == value || !acceptParameter(
                value, minimum, maximum,
                QT_TRANSLATE_NOOP("EditController", "Point Color")
            )) {
            return;
        }
        const BackendGradeStack before = grade_stack_;
        if (parameter_key == QStringLiteral("color_range_enabled")) range.enabled = value >= 0.5;
        else if (parameter_key == QStringLiteral("color_range_center")) range.center_degrees = value;
        else if (parameter_key == QStringLiteral("color_range_width")) range.width_degrees = value;
        else if (parameter_key == QStringLiteral("color_range_softness")) range.softness = value;
        else if (parameter_key == QStringLiteral("color_range_hue")) range.hue_shift_degrees = value;
        else if (parameter_key == QStringLiteral("color_range_saturation")) range.saturation = value;
        else if (parameter_key == QStringLiteral("color_range_lightness")) range.lightness = value;
        set_point_color_at(fine, selected_point_color_index_, range);
        parameterEdited(
            QStringLiteral("point_color/%1/%2").arg(selected_point_color_index_).arg(parameter_key),
            before
        );
        return;
    }
    double minimum = -1.0;
    double maximum = 1.0;
    const char* label = QT_TRANSLATE_NOOP("EditController", "Adjustment");
    double* target = nullptr;
    if (parameter_key == QStringLiteral("highlights")) {
        target = &fine.highlights;
        label = QT_TRANSLATE_NOOP("EditController", "Highlights");
    } else if (parameter_key == QStringLiteral("shadows")) {
        target = &fine.shadows;
        label = QT_TRANSLATE_NOOP("EditController", "Shadows");
    } else if (parameter_key == QStringLiteral("whites")) {
        target = &fine.whites;
        label = QT_TRANSLATE_NOOP("EditController", "Whites");
    } else if (parameter_key == QStringLiteral("blacks")) {
        target = &fine.blacks;
        label = QT_TRANSLATE_NOOP("EditController", "Blacks");
    } else if (parameter_key == QStringLiteral("global_a_balance")) {
        target = &fine.global_a_balance;
        label = QT_TRANSLATE_NOOP("EditController", "Green to red balance");
    } else if (parameter_key == QStringLiteral("global_b_balance")) {
        target = &fine.global_b_balance;
        label = QT_TRANSLATE_NOOP("EditController", "Blue to yellow balance");
    } else if (parameter_key == QStringLiteral("vibrance")) {
        target = &fine.vibrance;
        label = QT_TRANSLATE_NOOP("EditController", "Vibrance");
    } else if (parameter_key == QStringLiteral("color_warper_strength")) {
        target = &fine.oklab_color_warper_strength;
        minimum = 0.0;
        maximum = 1.0;
        label = QT_TRANSLATE_NOOP("EditController", "Color Warper strength");
    } else if (parameter_key == QStringLiteral("lut_intensity")) {
        target = &fine.lut_intensity;
        minimum = 0.0;
        maximum = 1.0;
        label = QT_TRANSLATE_NOOP("EditController", "LUT intensity");
    } else if (parameter_key == QStringLiteral("sharpen_amount")) {
        target = &fine.sharpen_amount;
        minimum = 0.0;
        maximum = 2.0;
        label = QT_TRANSLATE_NOOP("EditController", "Sharpening amount");
    } else if (parameter_key == QStringLiteral("sharpen_radius")) {
        target = &fine.sharpen_radius;
        minimum = 0.1;
        maximum = 5.0;
        label = QT_TRANSLATE_NOOP("EditController", "Sharpening radius");
    } else if (parameter_key == QStringLiteral("sharpen_threshold")) {
        target = &fine.sharpen_threshold;
        minimum = 0.0;
        maximum = 1.0;
        label = QT_TRANSLATE_NOOP("EditController", "Sharpening threshold");
    } else if (parameter_key == QStringLiteral("sharpen_masking")) {
        target = &fine.sharpen_masking;
        minimum = 0.0;
        maximum = 1.0;
        label = QT_TRANSLATE_NOOP("EditController", "Sharpening masking");
    } else if (parameter_key == QStringLiteral("clarity")) {
        target = &fine.clarity;
        label = QT_TRANSLATE_NOOP("EditController", "Perceptual clarity");
    } else if (parameter_key == QStringLiteral("texture")) {
        target = &fine.texture;
        label = QT_TRANSLATE_NOOP("EditController", "Perceptual texture");
    } else if (parameter_key == QStringLiteral("local_contrast")) {
        target = &fine.local_contrast;
        label = QT_TRANSLATE_NOOP("EditController", "Local contrast");
    } else if (parameter_key == QStringLiteral("local_contrast_scale")) {
        target = &fine.local_contrast_scale;
        minimum = 0.0;
        maximum = 1.0;
        label = QT_TRANSLATE_NOOP("EditController", "Local contrast scale");
    } else if (parameter_key == QStringLiteral("selective_color_lightness_protection")) {
        target = &fine.selective_color_lightness_protection;
        minimum = 0.0;
        maximum = 1.0;
        label = QT_TRANSLATE_NOOP("EditController", "Selective Color lightness protection");
    } else if (parameter_key == QStringLiteral("denoise_luminance")) {
        target = &fine.denoise_luminance; minimum = 0.0; maximum = 1.0;
        label = QT_TRANSLATE_NOOP("EditController", "Luminance noise reduction");
    } else if (parameter_key == QStringLiteral("denoise_detail")) {
        target = &fine.denoise_detail; minimum = 0.0; maximum = 1.0;
        label = QT_TRANSLATE_NOOP("EditController", "Noise reduction detail");
    } else if (parameter_key == QStringLiteral("denoise_color")) {
        target = &fine.denoise_color; minimum = 0.0; maximum = 1.0;
        label = QT_TRANSLATE_NOOP("EditController", "Color noise reduction");
    } else if (parameter_key == QStringLiteral("dehaze")) {
        target = &fine.dehaze;
        label = QT_TRANSLATE_NOOP("EditController", "Dehaze");
    } else if (parameter_key == QStringLiteral("defringe_purple_amount")) {
        target = &fine.defringe_purple_amount; minimum = 0.0; maximum = 1.0;
        label = QT_TRANSLATE_NOOP("EditController", "Purple defringe amount");
    } else if (parameter_key == QStringLiteral("defringe_green_amount")) {
        target = &fine.defringe_green_amount; minimum = 0.0; maximum = 1.0;
        label = QT_TRANSLATE_NOOP("EditController", "Green defringe amount");
    } else if (parameter_key == QStringLiteral("shadows_hue")) {
        target = &fine.shadows_hue; minimum = 0.0; maximum = 360.0;
        label = QT_TRANSLATE_NOOP("EditController", "Shadow grading hue");
    } else if (parameter_key == QStringLiteral("shadows_saturation")) {
        target = &fine.shadows_saturation; minimum = 0.0; maximum = 1.0;
        label = QT_TRANSLATE_NOOP("EditController", "Shadow grading saturation");
    } else if (parameter_key == QStringLiteral("shadows_luminance")) {
        target = &fine.shadows_luminance;
        label = QT_TRANSLATE_NOOP("EditController", "Shadow grading luminance");
    } else if (parameter_key == QStringLiteral("midtones_hue")) {
        target = &fine.midtones_hue; minimum = 0.0; maximum = 360.0;
        label = QT_TRANSLATE_NOOP("EditController", "Midtone grading hue");
    } else if (parameter_key == QStringLiteral("midtones_saturation")) {
        target = &fine.midtones_saturation; minimum = 0.0; maximum = 1.0;
        label = QT_TRANSLATE_NOOP("EditController", "Midtone grading saturation");
    } else if (parameter_key == QStringLiteral("midtones_luminance")) {
        target = &fine.midtones_luminance;
        label = QT_TRANSLATE_NOOP("EditController", "Midtone grading luminance");
    } else if (parameter_key == QStringLiteral("highlights_hue")) {
        target = &fine.highlights_hue; minimum = 0.0; maximum = 360.0;
        label = QT_TRANSLATE_NOOP("EditController", "Highlight grading hue");
    } else if (parameter_key == QStringLiteral("highlights_saturation")) {
        target = &fine.highlights_saturation; minimum = 0.0; maximum = 1.0;
        label = QT_TRANSLATE_NOOP("EditController", "Highlight grading saturation");
    } else if (parameter_key == QStringLiteral("highlights_luminance")) {
        target = &fine.highlights_luminance;
        label = QT_TRANSLATE_NOOP("EditController", "Highlight grading luminance");
    } else if (parameter_key == QStringLiteral("grading_blending")) {
        target = &fine.grading_blending; minimum = 0.0; maximum = 1.0;
        label = QT_TRANSLATE_NOOP("EditController", "Color grading blending");
    } else if (parameter_key == QStringLiteral("grading_balance")) {
        target = &fine.grading_balance;
        label = QT_TRANSLATE_NOOP("EditController", "Color grading balance");
    } else if (parameter_key == QStringLiteral("grain_amount")) {
        target = &fine.grain_amount; minimum = 0.0; maximum = 1.0;
        label = QT_TRANSLATE_NOOP("EditController", "Grain amount");
    } else if (parameter_key == QStringLiteral("grain_size")) {
        target = &fine.grain_size; minimum = 0.0; maximum = 1.0;
        label = QT_TRANSLATE_NOOP("EditController", "Grain size");
    } else if (parameter_key == QStringLiteral("grain_roughness")) {
        target = &fine.grain_roughness; minimum = 0.0; maximum = 1.0;
        label = QT_TRANSLATE_NOOP("EditController", "Grain roughness");
    } else if (parameter_key == QStringLiteral("vignette_amount")) {
        target = &fine.vignette_amount;
        label = QT_TRANSLATE_NOOP("EditController", "Vignette amount");
    } else if (parameter_key == QStringLiteral("vignette_midpoint")) {
        target = &fine.vignette_midpoint; minimum = 0.0; maximum = 1.0;
        label = QT_TRANSLATE_NOOP("EditController", "Vignette midpoint");
    } else if (parameter_key == QStringLiteral("vignette_roundness")) {
        target = &fine.vignette_roundness;
        label = QT_TRANSLATE_NOOP("EditController", "Vignette roundness");
    } else if (parameter_key == QStringLiteral("vignette_feather")) {
        target = &fine.vignette_feather; minimum = 0.0; maximum = 1.0;
        label = QT_TRANSLATE_NOOP("EditController", "Vignette feather");
    } else if (parameter_key == QStringLiteral("vignette_highlights")) {
        target = &fine.vignette_highlights; minimum = 0.0; maximum = 1.0;
        label = QT_TRANSLATE_NOOP("EditController", "Vignette highlights");
    } else if (parameter_key == QStringLiteral("color_range_center")) {
        target = &fine.color_range_center;
        minimum = 0.0;
        maximum = 360.0;
        label = QT_TRANSLATE_NOOP("EditController", "Color range hue");
    } else if (parameter_key == QStringLiteral("color_range_width")) {
        target = &fine.color_range_width;
        minimum = 1.0;
        maximum = 180.0;
        label = QT_TRANSLATE_NOOP("EditController", "Color range width");
    } else if (parameter_key == QStringLiteral("color_range_softness")) {
        target = &fine.color_range_softness;
        minimum = 0.0;
        maximum = 1.0;
        label = QT_TRANSLATE_NOOP("EditController", "Color range softness");
    } else if (parameter_key == QStringLiteral("color_range_hue")) {
        target = &fine.color_range_hue;
        minimum = -180.0;
        maximum = 180.0;
        label = QT_TRANSLATE_NOOP("EditController", "Color range hue shift");
    } else if (parameter_key == QStringLiteral("color_range_saturation")) {
        target = &fine.color_range_saturation;
        label = QT_TRANSLATE_NOOP("EditController", "Color range saturation");
    } else if (parameter_key == QStringLiteral("color_range_lightness")) {
        target = &fine.color_range_lightness;
        label = QT_TRANSLATE_NOOP("EditController", "Color range lightness");
    } else if (parameter_key == QStringLiteral("color_range_enabled")) {
        if (!acceptParameter(
                value,
                0.0,
                1.0,
                QT_TRANSLATE_NOOP("EditController", "Color range")
            )) {
            return;
        }
        const bool enabled = value >= 0.5;
        if (fine.color_range_enabled == enabled) {
            return;
        }
        const BackendGradeStack before = grade_stack_;
        fine.color_range_enabled = enabled;
        parameterEdited(parameter_key, before);
        return;
    } else {
        return;
    }

    if (target == nullptr || *target == value
        || !acceptParameter(value, minimum, maximum, label)) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    *target = value;
    parameterEdited(parameter_key, before);
}

void EditController::setColorMixerValue(
    const int band_index,
    const QString& component,
    const double value
) {
    if (band_index < 0
        || static_cast<std::size_t>(band_index) >= BACKEND_COLOR_MIXER_BAND_COUNT
        || !acceptParameter(
            value,
            -1.0,
            1.0,
            QT_TRANSLATE_NOOP("EditController", "Color Mixer")
        )) {
        return;
    }
    auto& fine = grade_stack_.grade_nodes[selected_grade_node_index_].fine;
    const std::size_t index = static_cast<std::size_t>(band_index);
    double* target = nullptr;
    if (component == QStringLiteral("hue")) {
        target = &fine.mixer_hue[index];
    } else if (component == QStringLiteral("saturation")) {
        target = &fine.mixer_saturation[index];
    } else if (component == QStringLiteral("lightness")) {
        target = &fine.mixer_lightness[index];
    }
    if (target == nullptr || *target == value) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    *target = value;
    parameterEdited(
        QStringLiteral("color_mixer/%1/%2").arg(component).arg(band_index),
        before
    );
}

void EditController::setColorWarperControlPoint(
    const int index,
    const double a_offset,
    const double b_offset
) {
    const auto* const selected = selectedGradeNode();
    if (!active_ || interactionLocked() || selected == nullptr || !selected->enabled
        || index < 0
        || static_cast<std::size_t>(index) >= BACKEND_OKLAB_COLOR_WARPER_CONTROL_POINT_COUNT
        || !std::isfinite(a_offset)
        || !std::isfinite(b_offset)
        || a_offset < -BACKEND_OKLAB_COLOR_WARPER_MAXIMUM_OFFSET
        || a_offset > BACKEND_OKLAB_COLOR_WARPER_MAXIMUM_OFFSET
        || b_offset < -BACKEND_OKLAB_COLOR_WARPER_MAXIMUM_OFFSET
        || b_offset > BACKEND_OKLAB_COLOR_WARPER_MAXIMUM_OFFSET
        || !acceptParameter(
            a_offset,
            -BACKEND_OKLAB_COLOR_WARPER_MAXIMUM_OFFSET,
            BACKEND_OKLAB_COLOR_WARPER_MAXIMUM_OFFSET,
            QT_TRANSLATE_NOOP("EditController", "Color Warper control point")
        )) {
        return;
    }
    auto& point = grade_stack_.grade_nodes[selected_grade_node_index_]
                      .fine
                      .oklab_color_warper_control_points[static_cast<std::size_t>(index)];
    if (point.a_offset == a_offset && point.b_offset == b_offset) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    point = {.a_offset = a_offset, .b_offset = b_offset};
    parameterEdited(QStringLiteral("color_warper/point/%1").arg(index), before);
}

void EditController::resetColorWarper() {
    const auto* const selected = selectedGradeNode();
    if (!active_ || interactionLocked() || selected == nullptr || !selected->enabled) {
        return;
    }
    auto& fine = grade_stack_.grade_nodes[selected_grade_node_index_].fine;
    const auto neutral_points = decltype(fine.oklab_color_warper_control_points){};
    if (fine.oklab_color_warper_control_points == neutral_points
        && fine.oklab_color_warper_strength == 1.0) {
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    fine.oklab_color_warper_control_points = neutral_points;
    fine.oklab_color_warper_strength = 1.0;
    parameterEdited(QStringLiteral("color_warper/reset"), before);
}

double EditController::selectiveColorValue(
    const int target_index,
    const int component_index
) const {
    const auto* const grade_node = selectedGradeNode();
    if (grade_node == nullptr || target_index < 0
        || static_cast<std::size_t>(target_index) >= BACKEND_SELECTIVE_COLOR_TARGET_COUNT
        || component_index < 0
        || static_cast<std::size_t>(component_index)
            >= BACKEND_SELECTIVE_COLOR_COMPONENT_COUNT) {
        return 0.0;
    }
    const std::size_t index = static_cast<std::size_t>(target_index)
        * BACKEND_SELECTIVE_COLOR_COMPONENT_COUNT
        + static_cast<std::size_t>(component_index);
    return grade_node->fine.selective_color_cmyk[index];
}

void EditController::setSelectiveColorValue(
    const int target_index,
    const int component_index,
    const double value
) {
    if (target_index < 0
        || static_cast<std::size_t>(target_index) >= BACKEND_SELECTIVE_COLOR_TARGET_COUNT
        || component_index < 0
        || static_cast<std::size_t>(component_index)
            >= BACKEND_SELECTIVE_COLOR_COMPONENT_COUNT
        || !acceptParameter(
            value,
            -1.0,
            1.0,
            QT_TRANSLATE_NOOP("EditController", "Selective Color")
        )) {
        return;
    }
    auto& fine = grade_stack_.grade_nodes[selected_grade_node_index_].fine;
    const std::size_t index = static_cast<std::size_t>(target_index)
        * BACKEND_SELECTIVE_COLOR_COMPONENT_COUNT
        + static_cast<std::size_t>(component_index);
    if (fine.selective_color_cmyk[index] == value) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    fine.selective_color_cmyk[index] = value;
    parameterEdited(
        QStringLiteral("selective_color/%1/%2").arg(target_index).arg(component_index),
        before
    );
}

bool EditController::selectiveColorRelative() const noexcept {
    const auto* const grade_node = selectedGradeNode();
    return grade_node == nullptr || grade_node->fine.selective_color_relative;
}

void EditController::setSelectiveColorRelative(const bool relative) {
    auto* const grade_node = selected_grade_node_index_ < 0
        ? nullptr : &grade_stack_.grade_nodes[selected_grade_node_index_];
    if (grade_node == nullptr || grade_node->fine.selective_color_relative == relative) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    grade_stack_.grade_nodes[selected_grade_node_index_].fine.selective_color_relative = relative;
    parameterEdited(QStringLiteral("selective_color/method"), before);
}

void EditController::selectPointColor(const int index) {
    const auto* const grade_node = selectedGradeNode();
    if (grade_node == nullptr || index < 0 || index >= point_color_count(grade_node->fine)
        || index == selected_point_color_index_) {
        return;
    }
    finishActiveGesture();
    selected_point_color_index_ = index;
    clearPointColorScopeReference();
    notifyParametersChanged();
}

void EditController::setPointColorScopeActive(const bool active) {
    const bool next = active && pointColorScopeAvailable();
    if (point_color_scope_active_ == next) {
        return;
    }
    point_color_scope_active_ = next;
    clearPointColorScopeReference();
    refreshCurrentDisplayScope();
    emit pointColorScopeChanged();
}

void EditController::clearPointColorScopeReference() noexcept {
    point_color_scope_reference_.reset();
}

void EditController::removeSelectedPointColor() {
    if (!active_ || interactionLocked()) {
        return;
    }
    auto* const grade_node = selected_grade_node_index_ < 0
        ? nullptr : &grade_stack_.grade_nodes[selected_grade_node_index_];
    if (grade_node == nullptr || selected_point_color_index_ < 0
        || selected_point_color_index_ >= point_color_count(grade_node->fine)) {
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    auto& fine = grade_node->fine;
    if (selected_point_color_index_ == 0) {
        if (!fine.additional_point_colors.isEmpty()) {
            const BackendPointColorRange promoted = fine.additional_point_colors.takeFirst();
            set_point_color_at(fine, 0, promoted);
        } else {
            fine.color_range_enabled = false;
            fine.color_range_center = 0.0;
            fine.color_range_width = 30.0;
            fine.color_range_softness = 0.5;
            fine.color_range_hue = 0.0;
            fine.color_range_saturation = 0.0;
            fine.color_range_lightness = 0.0;
        }
    } else {
        fine.additional_point_colors.removeAt(selected_point_color_index_ - 1);
    }
    const int count = point_color_count(fine);
    selected_point_color_index_ = count == 0
        ? -1 : std::min(selected_point_color_index_, count - 1);
    clearPointColorScopeReference();
    parameterEdited(QStringLiteral("point_color/remove"), before);
}

void EditController::setPointColorPickerActive(const bool active) {
    if (point_color_picker_active_ == active) {
        return;
    }
    point_color_picker_active_ = active;
    emit pointColorPickerActiveChanged();
    if (active && white_balance_picker_active_) {
        white_balance_picker_active_ = false;
        emit whiteBalancePickerActiveChanged();
    }
    if (active && retouch_picker_active_) {
        endRetouchStroke();
        retouch_picker_active_ = false;
        emit retouchPickerActiveChanged();
    }
}

void EditController::setRetouchPickerActive(const bool active) {
    if (!active) {
        endRetouchStroke();
    }
    if (retouch_picker_active_ == active) {
        return;
    }
    retouch_picker_active_ = active;
    emit retouchPickerActiveChanged();
    if (active && point_color_picker_active_) {
        point_color_picker_active_ = false;
        emit pointColorPickerActiveChanged();
    }
    if (active && white_balance_picker_active_) {
        white_balance_picker_active_ = false;
        emit whiteBalancePickerActiveChanged();
    }
}

void EditController::setRetouchCreationMode(const int mode) {
    constexpr int heal_mode = 0;
    constexpr int clone_mode = 1;
    if ((mode != heal_mode && mode != clone_mode)
        || retouch_creation_mode_ == mode) {
        return;
    }
    retouch_creation_mode_ = mode;
    emit retouchCreationModeChanged();
}

void EditController::setWhiteBalancePickerActive(const bool active) {
    if (white_balance_picker_active_ == active) {
        return;
    }
    white_balance_picker_active_ = active;
    emit whiteBalancePickerActiveChanged();
    if (active && point_color_picker_active_) {
        point_color_picker_active_ = false;
        emit pointColorPickerActiveChanged();
    }
    if (active && retouch_picker_active_) {
        endRetouchStroke();
        retouch_picker_active_ = false;
        emit retouchPickerActiveChanged();
    }
}

void EditController::setWhiteBalanceFromPreview(
    const double normalized_x,
    const double normalized_y,
    const QString& preview_generation
) {
    if (!active_ || interactionLocked() || !hasSelectedGradeNode()
        || !std::isfinite(normalized_x) || !std::isfinite(normalized_y)
        || normalized_x < 0.0 || normalized_x > 1.0
        || normalized_y < 0.0 || normalized_y > 1.0) {
        return;
    }
    bool valid_generation = false;
    const quint64 visible_generation = preview_generation.toULongLong(&valid_generation);
    if (!valid_generation) {
        setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
            "EditController", "White Balance needs a ready preview"
        )));
        return;
    }
    const auto snapshot = preview_store_->snapshot(
        EditPreviewSlot::Current, visible_generation
    );
    const QImage image = QImage::fromData(snapshot.bytes);
    if (image.isNull()) {
        setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
            "EditController", "White Balance needs a ready preview"
        )));
        return;
    }
    const int center_x = std::clamp(
        static_cast<int>(std::round(normalized_x * (image.width() - 1))),
        0, image.width() - 1
    );
    const int center_y = std::clamp(
        static_cast<int>(std::round(normalized_y * (image.height() - 1))),
        0, image.height() - 1
    );
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    int samples = 0;
    for (int y = std::max(0, center_y - 1);
         y <= std::min(image.height() - 1, center_y + 1); ++y) {
        for (int x = std::max(0, center_x - 1);
             x <= std::min(image.width() - 1, center_x + 1); ++x) {
            const QColor sample = image.pixelColor(x, y);
            red += sample.redF();
            green += sample.greenF();
            blue += sample.blueF();
            ++samples;
        }
    }
    const double inverse = 1.0 / static_cast<double>(samples);
    red = std::max(red * inverse, 1.0e-4);
    green = std::max(green * inverse, 1.0e-4);
    blue = std::max(blue * inverse, 1.0e-4);

    // The preview is already rendered with the current node. Convert the
    // sampled neutral error into an additive correction in the normalized
    // temperature/tint intent space. The image kernel remains authoritative
    // for the CAT16 transform applied by those parameters.
    const auto* const grade_node = selectedGradeNode();
    const double temperature_delta = -0.9 * std::log(red / blue);
    const double tint_delta = 0.9 * std::log(green / std::sqrt(red * blue));
    const BackendGradeStack before = grade_stack_;
    auto& basic = grade_stack_.grade_nodes[selected_grade_node_index_].basic;
    basic.white_balance_temperature = std::clamp(
        grade_node->basic.white_balance_temperature + temperature_delta,
        -1.0,
        1.0
    );
    basic.white_balance_tint = std::clamp(
        grade_node->basic.white_balance_tint + tint_delta,
        -1.0,
        1.0
    );
    setWhiteBalancePickerActive(false);
    parameterEdited(QStringLiteral("white_balance/picker"), before);
}

void EditController::addPointColorFromPreview(
    const double normalized_x,
    const double normalized_y,
    const QString& preview_generation
) {
    if (!active_ || interactionLocked() || !std::isfinite(normalized_x)
        || !std::isfinite(normalized_y) || normalized_x < 0.0 || normalized_x > 1.0
        || normalized_y < 0.0 || normalized_y > 1.0) {
        return;
    }
    bool valid_generation = false;
    const quint64 visible_generation = preview_generation.toULongLong(&valid_generation);
    if (!valid_generation) {
        setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
            "EditController", "Point Color needs a ready preview"
        )));
        return;
    }
    const auto snapshot = preview_store_->snapshot(
        EditPreviewSlot::Current, visible_generation
    );
    const QImage image = QImage::fromData(snapshot.bytes);
    if (image.isNull()) {
        setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
            "EditController", "Point Color needs a ready preview"
        )));
        return;
    }
    const int center_x = std::clamp(
        static_cast<int>(std::round(normalized_x * (image.width() - 1))),
        0, image.width() - 1
    );
    const int center_y = std::clamp(
        static_cast<int>(std::round(normalized_y * (image.height() - 1))),
        0, image.height() - 1
    );
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    int samples = 0;
    for (int y = std::max(0, center_y - 1); y <= std::min(image.height() - 1, center_y + 1); ++y) {
        for (int x = std::max(0, center_x - 1); x <= std::min(image.width() - 1, center_x + 1); ++x) {
            const QColor sample = image.pixelColor(x, y);
            red += sample.redF();
            green += sample.greenF();
            blue += sample.blueF();
            ++samples;
        }
    }
    const double inverse = 1.0 / static_cast<double>(samples);
    const BackendPointColorRange range{
        .enabled = true,
        .center_degrees = sampled_oklch_hue(QColor::fromRgbF(
            static_cast<float>(red * inverse),
            static_cast<float>(green * inverse),
            static_cast<float>(blue * inverse)
        )),
    };
    auto& fine = grade_stack_.grade_nodes[selected_grade_node_index_].fine;
    if (point_color_count(fine) >= MAX_POINT_COLOR_COUNT) {
        setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
            "EditController", "Point Color supports at most 16 samples"
        )));
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    if (point_color_count(fine) == 0) {
        set_point_color_at(fine, 0, range);
        selected_point_color_index_ = 0;
    } else {
        fine.additional_point_colors.push_back(range);
        selected_point_color_index_ = point_color_count(fine) - 1;
    }
    clearPointColorScopeReference();
    parameterEdited(QStringLiteral("point_color/add"), before);
    setPointColorPickerActive(false);
}

void EditController::addRetouchSpotFromPreview(
    const double normalized_x,
    const double normalized_y
) {
    if (!active_ || interactionLocked() || !retouch_picker_active_
        || !std::isfinite(normalized_x) || !std::isfinite(normalized_y)
        || normalized_x < 0.0 || normalized_x > 1.0
        || normalized_y < 0.0 || normalized_y > 1.0) {
        return;
    }
    constexpr qsizetype maximum_retouch_spots = 64;
    if (grade_stack_.retouch_spots.size() >= maximum_retouch_spots) {
        setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
            "EditController", "Repair supports at most 64 spots"
        )));
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    grade_stack_.retouch_spots.push_back(BackendRetouchSpot{
        .center_x = normalized_x,
        .center_y = normalized_y,
        .radius_level_zero_pixels = 18U,
        .mode = static_cast<std::uint8_t>(retouch_creation_mode_),
        .source_offset_x_radii = retouch_creation_mode_ == 1 ? 1.5 : 0.0,
        .source_offset_y_radii = retouch_creation_mode_ == 1 ? -1.0 : 0.0,
        .feather = 0.28,
    });
    parameterEdited(QStringLiteral("retouch/add"), before);
    setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
        "EditController", "Added repair spot"
    )));
}

void EditController::beginRetouchStroke(
    const double normalized_x,
    const double normalized_y
) {
    if (!active_ || interactionLocked() || !retouch_picker_active_
        || active_retouch_stroke_index_ >= 0
        || !std::isfinite(normalized_x) || !std::isfinite(normalized_y)
        || normalized_x < 0.0 || normalized_x > 1.0
        || normalized_y < 0.0 || normalized_y > 1.0) {
        return;
    }
    constexpr qsizetype maximum_retouch_strokes = 64;
    if (grade_stack_.retouch_strokes.size() >= maximum_retouch_strokes) {
        setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
            "EditController", "Repair supports at most 64 strokes"
        )));
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    const QString key = QStringLiteral("retouch/stroke/add");
    beginParameterEdit(key);
    grade_stack_.retouch_strokes.push_back(BackendRetouchStroke{
        .points = {{.x = normalized_x, .y = normalized_y}},
        .radius_level_zero_pixels = 18U,
        .mode = static_cast<std::uint8_t>(retouch_creation_mode_),
        .source_offset_x_radii = retouch_creation_mode_ == 1 ? 1.5 : 0.0,
        .source_offset_y_radii = retouch_creation_mode_ == 1 ? -1.0 : 0.0,
        .feather = 0.28,
    });
    active_retouch_stroke_index_ = static_cast<int>(
        grade_stack_.retouch_strokes.size() - 1
    );
    parameterEdited(key, before);
}

void EditController::appendRetouchStrokePoint(
    const double normalized_x,
    const double normalized_y
) {
    constexpr qsizetype maximum_retouch_stroke_points = 512;
    if (!active_ || interactionLocked() || active_retouch_stroke_index_ < 0
        || active_retouch_stroke_index_ >= grade_stack_.retouch_strokes.size()
        || !std::isfinite(normalized_x) || !std::isfinite(normalized_y)
        || normalized_x < 0.0 || normalized_x > 1.0
        || normalized_y < 0.0 || normalized_y > 1.0) {
        return;
    }
    auto& stroke = grade_stack_.retouch_strokes[active_retouch_stroke_index_];
    if (!stroke.points.isEmpty()
        && stroke.points.back().x == normalized_x
        && stroke.points.back().y == normalized_y) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    if (stroke.points.size() >= maximum_retouch_stroke_points) {
        // Keep one gesture continuous even on very long drags. The persistent
        // contract stays bounded, while downsampling the already-swept path
        // is preferable to silently dropping the rest of the painted region.
        QVector<BackendRetouchStrokePoint> compacted;
        compacted.reserve((stroke.points.size() + 1) / 2);
        for (qsizetype index = 0; index < stroke.points.size(); index += 2) {
            compacted.push_back(stroke.points.at(index));
        }
        stroke.points = std::move(compacted);
    }
    stroke.points.push_back({.x = normalized_x, .y = normalized_y});
    parameterEdited(QStringLiteral("retouch/stroke/add"), before);
}

void EditController::endRetouchStroke() {
    if (active_retouch_stroke_index_ < 0) {
        return;
    }
    active_retouch_stroke_index_ = -1;
    endParameterEdit(QStringLiteral("retouch/stroke/add"));
}

void EditController::setRetouchSpotCenter(
    const int index,
    const double normalized_x,
    const double normalized_y
) {
    if (!active_ || interactionLocked() || index < 0
        || index >= grade_stack_.retouch_spots.size()
        || !std::isfinite(normalized_x) || !std::isfinite(normalized_y)
        || normalized_x < 0.0 || normalized_x > 1.0
        || normalized_y < 0.0 || normalized_y > 1.0) {
        return;
    }
    auto& spot = grade_stack_.retouch_spots[index];
    if (spot.center_x == normalized_x && spot.center_y == normalized_y) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    spot.center_x = normalized_x;
    spot.center_y = normalized_y;
    parameterEdited(QStringLiteral("retouch/%1/center").arg(index), before);
}

void EditController::setRetouchSpotRadius(
    const int index,
    const int radius_level_zero_pixels
) {
    constexpr int minimum_radius = 1;
    constexpr int maximum_radius = 128;
    if (!active_ || interactionLocked() || index < 0
        || index >= grade_stack_.retouch_spots.size()
        || radius_level_zero_pixels < minimum_radius
        || radius_level_zero_pixels > maximum_radius) {
        return;
    }
    auto& spot = grade_stack_.retouch_spots[index];
    const auto radius = static_cast<std::uint16_t>(radius_level_zero_pixels);
    if (spot.radius_level_zero_pixels == radius) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    spot.radius_level_zero_pixels = radius;
    parameterEdited(QStringLiteral("retouch/%1/radius").arg(index), before);
}

void EditController::setRetouchSpotMode(const int index, const int mode) {
    constexpr int heal_mode = 0;
    constexpr int clone_mode = 1;
    if (!active_ || interactionLocked() || index < 0
        || index >= grade_stack_.retouch_spots.size()
        || (mode != heal_mode && mode != clone_mode)) {
        return;
    }
    auto& spot = grade_stack_.retouch_spots[index];
    const auto encoded_mode = static_cast<std::uint8_t>(mode);
    if (spot.mode == encoded_mode) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    spot.mode = encoded_mode;
    if (mode == clone_mode
        && spot.source_offset_x_radii == 0.0
        && spot.source_offset_y_radii == 0.0) {
        spot.source_offset_x_radii = 1.5;
        spot.source_offset_y_radii = -1.0;
    }
    parameterEdited(QStringLiteral("retouch/%1/mode").arg(index), before);
}

void EditController::setRetouchSpotFeather(const int index, const double feather) {
    if (!active_ || interactionLocked() || index < 0
        || index >= grade_stack_.retouch_spots.size()
        || !std::isfinite(feather) || feather < 0.0 || feather > 1.0) {
        return;
    }
    auto& spot = grade_stack_.retouch_spots[index];
    if (spot.feather == feather) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    spot.feather = feather;
    parameterEdited(QStringLiteral("retouch/%1/feather").arg(index), before);
}

void EditController::setRetouchSpotSourceOffset(
    const int index,
    const double offset_x_radii,
    const double offset_y_radii
) {
    constexpr double maximum_offset_radii = 2.0;
    if (!active_ || interactionLocked() || index < 0
        || index >= grade_stack_.retouch_spots.size()
        || !std::isfinite(offset_x_radii) || !std::isfinite(offset_y_radii)
        || offset_x_radii < -maximum_offset_radii
        || offset_x_radii > maximum_offset_radii
        || offset_y_radii < -maximum_offset_radii
        || offset_y_radii > maximum_offset_radii) {
        return;
    }
    auto& spot = grade_stack_.retouch_spots[index];
    if (spot.source_offset_x_radii == offset_x_radii
        && spot.source_offset_y_radii == offset_y_radii) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    spot.source_offset_x_radii = offset_x_radii;
    spot.source_offset_y_radii = offset_y_radii;
    parameterEdited(QStringLiteral("retouch/%1/source").arg(index), before);
}

void EditController::removeRetouchSpot(const int index) {
    if (!active_ || interactionLocked() || index < 0
        || index >= grade_stack_.retouch_spots.size()) {
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    grade_stack_.retouch_spots.removeAt(index);
    parameterEdited(QStringLiteral("retouch/remove"), before);
    setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
        "EditController", "Removed repair spot"
    )));
}

void EditController::setRetouchStrokeRadius(
    const int index,
    const int radius_level_zero_pixels
) {
    constexpr int minimum_radius = 1;
    constexpr int maximum_radius = 128;
    if (!active_ || interactionLocked() || index < 0
        || index >= grade_stack_.retouch_strokes.size()
        || radius_level_zero_pixels < minimum_radius
        || radius_level_zero_pixels > maximum_radius) {
        return;
    }
    auto& stroke = grade_stack_.retouch_strokes[index];
    const auto radius = static_cast<std::uint16_t>(radius_level_zero_pixels);
    if (stroke.radius_level_zero_pixels == radius) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    stroke.radius_level_zero_pixels = radius;
    parameterEdited(QStringLiteral("retouch/stroke/%1/radius").arg(index), before);
}

void EditController::setRetouchStrokeMode(const int index, const int mode) {
    constexpr int heal_mode = 0;
    constexpr int clone_mode = 1;
    if (!active_ || interactionLocked() || index < 0
        || index >= grade_stack_.retouch_strokes.size()
        || (mode != heal_mode && mode != clone_mode)) {
        return;
    }
    auto& stroke = grade_stack_.retouch_strokes[index];
    const auto encoded_mode = static_cast<std::uint8_t>(mode);
    if (stroke.mode == encoded_mode) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    stroke.mode = encoded_mode;
    if (mode == clone_mode
        && stroke.source_offset_x_radii == 0.0
        && stroke.source_offset_y_radii == 0.0) {
        stroke.source_offset_x_radii = 1.5;
        stroke.source_offset_y_radii = -1.0;
    }
    parameterEdited(QStringLiteral("retouch/stroke/%1/mode").arg(index), before);
}

void EditController::setRetouchStrokeFeather(const int index, const double feather) {
    if (!active_ || interactionLocked() || index < 0
        || index >= grade_stack_.retouch_strokes.size()
        || !std::isfinite(feather) || feather < 0.0 || feather > 1.0) {
        return;
    }
    auto& stroke = grade_stack_.retouch_strokes[index];
    if (stroke.feather == feather) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    stroke.feather = feather;
    parameterEdited(QStringLiteral("retouch/stroke/%1/feather").arg(index), before);
}

void EditController::setRetouchStrokeSourceOffset(
    const int index,
    const double offset_x_radii,
    const double offset_y_radii
) {
    constexpr double maximum_offset_radii = 2.0;
    if (!active_ || interactionLocked() || index < 0
        || index >= grade_stack_.retouch_strokes.size()
        || !std::isfinite(offset_x_radii) || !std::isfinite(offset_y_radii)
        || offset_x_radii < -maximum_offset_radii
        || offset_x_radii > maximum_offset_radii
        || offset_y_radii < -maximum_offset_radii
        || offset_y_radii > maximum_offset_radii) {
        return;
    }
    auto& stroke = grade_stack_.retouch_strokes[index];
    if (stroke.source_offset_x_radii == offset_x_radii
        && stroke.source_offset_y_radii == offset_y_radii) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    stroke.source_offset_x_radii = offset_x_radii;
    stroke.source_offset_y_radii = offset_y_radii;
    parameterEdited(QStringLiteral("retouch/stroke/%1/source").arg(index), before);
}

void EditController::removeRetouchStroke(const int index) {
    if (!active_ || interactionLocked() || index < 0
        || index >= grade_stack_.retouch_strokes.size()) {
        return;
    }
    finishActiveGesture();
    active_retouch_stroke_index_ = -1;
    const BackendGradeStack before = grade_stack_;
    grade_stack_.retouch_strokes.removeAt(index);
    parameterEdited(QStringLiteral("retouch/stroke/remove"), before);
}

void EditController::rotatePhotoClockwise() {
    if (!active_ || interactionLocked()) {
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    grade_stack_.geometry.quarter_turn = static_cast<std::uint8_t>(
        (static_cast<unsigned int>(grade_stack_.geometry.quarter_turn) + 1U) % 4U
    );
    setFullResolutionState(false, false, 0);
    parameterEdited(QStringLiteral("geometry/rotate_clockwise"), before);
}

void EditController::rotatePhotoCounterClockwise() {
    if (!active_ || interactionLocked()) {
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    grade_stack_.geometry.quarter_turn = static_cast<std::uint8_t>(
        (static_cast<unsigned int>(grade_stack_.geometry.quarter_turn) + 3U) % 4U
    );
    setFullResolutionState(false, false, 0);
    parameterEdited(QStringLiteral("geometry/rotate_counterclockwise"), before);
}

void EditController::flipPhotoHorizontally() {
    if (!active_ || interactionLocked()) {
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    const bool quarter_turn_is_odd = grade_stack_.geometry.quarter_turn % 2U != 0U;
    if (quarter_turn_is_odd) {
        grade_stack_.geometry.flip_vertical = !grade_stack_.geometry.flip_vertical;
    } else {
        grade_stack_.geometry.flip_horizontal = !grade_stack_.geometry.flip_horizontal;
    }
    setFullResolutionState(false, false, 0);
    parameterEdited(QStringLiteral("geometry/flip_horizontal"), before);
}

void EditController::flipPhotoVertically() {
    if (!active_ || interactionLocked()) {
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    const bool quarter_turn_is_odd = grade_stack_.geometry.quarter_turn % 2U != 0U;
    if (quarter_turn_is_odd) {
        grade_stack_.geometry.flip_horizontal = !grade_stack_.geometry.flip_horizontal;
    } else {
        grade_stack_.geometry.flip_vertical = !grade_stack_.geometry.flip_vertical;
    }
    setFullResolutionState(false, false, 0);
    parameterEdited(QStringLiteral("geometry/flip_vertical"), before);
}

void EditController::setCenteredPhotoCropAspectRatio(
    const double output_aspect_ratio,
    const double current_output_aspect_ratio
) {
    if (!active_ || interactionLocked() || !std::isfinite(output_aspect_ratio)
        || !std::isfinite(current_output_aspect_ratio) || output_aspect_ratio <= 0.0
        || current_output_aspect_ratio <= 0.0) {
        return;
    }
    constexpr double minimum_aspect_ratio = 1.0 / 8.0;
    constexpr double maximum_aspect_ratio = 8.0;
    if (output_aspect_ratio < minimum_aspect_ratio
        || output_aspect_ratio > maximum_aspect_ratio) {
        return;
    }

    // The canvas reports post-orientation dimensions. Crop is stored in the
    // original source coordinate system, so odd quarter-turns invert the
    // ratio before the centered crop is solved. That keeps “4:5” visually
    // consistent whether the photo was rotated before or after choosing it.
    const bool quarter_turn_is_odd = grade_stack_.geometry.quarter_turn % 2U != 0U;
    const double source_target_aspect = quarter_turn_is_odd
        ? 1.0 / output_aspect_ratio : output_aspect_ratio;
    const double source_current_aspect = quarter_turn_is_odd
        ? 1.0 / current_output_aspect_ratio : current_output_aspect_ratio;
    if (!std::isfinite(source_target_aspect) || !std::isfinite(source_current_aspect)
        || source_target_aspect <= 0.0 || source_current_aspect <= 0.0) {
        return;
    }

    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    auto& geometry = grade_stack_.geometry;
    const double width = geometry.crop_right - geometry.crop_left;
    const double height = geometry.crop_bottom - geometry.crop_top;
    if (source_current_aspect > source_target_aspect) {
        const double reduced_width = width * source_target_aspect / source_current_aspect;
        const double midpoint = (geometry.crop_left + geometry.crop_right) * 0.5;
        geometry.crop_left = midpoint - reduced_width * 0.5;
        geometry.crop_right = midpoint + reduced_width * 0.5;
    } else {
        const double reduced_height = height * source_current_aspect / source_target_aspect;
        const double midpoint = (geometry.crop_top + geometry.crop_bottom) * 0.5;
        geometry.crop_top = midpoint - reduced_height * 0.5;
        geometry.crop_bottom = midpoint + reduced_height * 0.5;
    }
    geometry.crop_left = std::clamp(geometry.crop_left, 0.0, 1.0);
    geometry.crop_top = std::clamp(geometry.crop_top, 0.0, 1.0);
    geometry.crop_right = std::clamp(geometry.crop_right, 0.0, 1.0);
    geometry.crop_bottom = std::clamp(geometry.crop_bottom, 0.0, 1.0);
    if (geometry == before.geometry) {
        return;
    }
    setFullResolutionState(false, false, 0);
    parameterEdited(QStringLiteral("geometry/crop/aspect"), before);
}

void EditController::setPhotoCropBounds(
    const double crop_left,
    const double crop_top,
    const double crop_right,
    const double crop_bottom
) {
    constexpr double minimum_crop_extent = 0.01;
    if (!active_ || interactionLocked()
        || !std::isfinite(crop_left) || !std::isfinite(crop_top)
        || !std::isfinite(crop_right) || !std::isfinite(crop_bottom)) {
        return;
    }

    const double left = std::clamp(crop_left, 0.0, 1.0);
    const double top = std::clamp(crop_top, 0.0, 1.0);
    const double right = std::clamp(crop_right, 0.0, 1.0);
    const double bottom = std::clamp(crop_bottom, 0.0, 1.0);
    if (right - left < minimum_crop_extent
        || bottom - top < minimum_crop_extent) {
        return;
    }

    const BackendGradeStack before = grade_stack_;
    auto& geometry = grade_stack_.geometry;
    geometry.crop_left = left;
    geometry.crop_top = top;
    geometry.crop_right = right;
    geometry.crop_bottom = bottom;
    if (geometry == before.geometry) {
        return;
    }

    setFullResolutionState(false, false, 0);
    recordWorkingTransition(
        gradeNodeHistoryKey(QStringLiteral("geometry/crop/bounds")),
        before
    );
    notifyParametersChanged();
    setDirty(version_draft_ || grade_stack_ != committed_grade_stack_);
    // While the crop tool is open the preview intentionally renders the
    // complete oriented source. Moving only the overlay must not launch
    // dozens of identical RAW renders; leaving the tool renders the crop.
    if (!crop_tool_active_) {
        schedulePreview(EDIT_PREVIEW_THROTTLE_MS);
    }
}

void EditController::setCropToolActive(const bool active) {
    if (crop_tool_active_ == active) {
        return;
    }
    finishActiveGesture();
    crop_tool_active_ = active;
    resetDetailState();
    first_interactive_frame_presented_ = false;
    cancelActivePreview(true);
    emit cropToolActiveChanged();
    if (active_) {
        schedulePreview(0);
    }
}

void EditController::setPhotoStraightenDegrees(const double degrees) {
    if (!active_ || interactionLocked() || !std::isfinite(degrees)
        || degrees < -45.0 || degrees > 45.0
        || grade_stack_.geometry.straighten_degrees == degrees) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    grade_stack_.geometry.straighten_degrees = degrees;
    setFullResolutionState(false, false, 0);
    parameterEdited(QStringLiteral("geometry/straighten"), before);
}

void EditController::resetPhotoGeometry() {
    if (!active_ || interactionLocked() || grade_stack_.geometry == BackendPhotoGeometry{}) {
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    grade_stack_.geometry = BackendPhotoGeometry{};
    setFullResolutionState(false, false, 0);
    parameterEdited(QStringLiteral("geometry/reset"), before);
}

void EditController::selectGradeNode(const int index) {
    const int count = static_cast<int>(grade_stack_.grade_nodes.size());
    if (!active_ || interactionLocked() || index < 0 || index >= count
        || index == selected_grade_node_index_) {
        return;
    }
    finishActiveGesture();
    setPointColorPickerActive(false);
    setRetouchPickerActive(false);
    setWhiteBalancePickerActive(false);
    setGradeStack(grade_stack_, grade_stack_.grade_nodes.at(index).grade_node_id);
}

void EditController::addGradeNode() {
    if (!canAddGradeNode()) {
    setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
        "EditController", "An edit can contain at most 16 Grade Nodes")));
        return;
    }
    finishActiveGesture();
    BackendGradeNode grade_node;
    try {
        grade_node = backend_->newBasicGradeNode(
            uniqueGradeNodeLabel(QStringLiteral("Adjustments"))
        );
    } catch (const std::exception& error) {
    setStatusMessage(edit_message(
        QT_TRANSLATE_NOOP("EditController",
                          "Could not create Grade Node · %1"),
        {QString::fromUtf8(error.what())}));
        return;
    }
    const BackendGradeStack before = grade_stack_;
    BackendGradeStack updated = grade_stack_;
    int selection = selected_grade_node_index_;
    if (!GradeNodeStack::insertAfterSelection(updated, grade_node, selection)) {
    setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
        "EditController",
        "The Grade Node could not be inserted safely")));
        return;
    }
    setGradeStack(std::move(updated), grade_node.grade_node_id);
    recordWorkingTransition(
        QStringLiteral("grade_node/%1/add").arg(grade_node.grade_node_id),
        before
    );
    schedulePreview(0);
  setStatusMessage(edit_message(
      QT_TRANSLATE_NOOP("EditController", "Added Grade Node · %1"),
      {display_grade_node_label(grade_node.label)}));
}

void EditController::duplicateSelectedGradeNode() {
    const auto* const source = selectedGradeNode();
    if (!canAddGradeNode() || source == nullptr) {
        return;
    }
    finishActiveGesture();
    BackendGradeNode duplicate;
    try {
        duplicate = backend_->newBasicGradeNode(
            uniqueGradeNodeLabel(source->label + QStringLiteral(" Copy"))
        );
    } catch (const std::exception& error) {
    setStatusMessage(edit_message(
        QT_TRANSLATE_NOOP("EditController",
                          "Could not duplicate Grade Node · %1"),
        {QString::fromUtf8(error.what())}));
        return;
    }
    duplicate.basic = source->basic;
    duplicate.fine = source->fine;
    duplicate.enabled = source->enabled;
    duplicate.local_mask_kind = source->local_mask_kind;
    duplicate.local_mask_x0 = source->local_mask_x0;
    duplicate.local_mask_y0 = source->local_mask_y0;
    duplicate.local_mask_x1 = source->local_mask_x1;
    duplicate.local_mask_y1 = source->local_mask_y1;
    duplicate.local_mask_radius_x = source->local_mask_radius_x;
    duplicate.local_mask_radius_y = source->local_mask_radius_y;
    duplicate.local_mask_feather = source->local_mask_feather;
    duplicate.local_mask_invert = source->local_mask_invert;
    duplicate.local_mask_brush_points = source->local_mask_brush_points;

    const BackendGradeStack before = grade_stack_;
    BackendGradeStack updated = grade_stack_;
    int selection = selected_grade_node_index_;
    if (!GradeNodeStack::insertAfterSelection(updated, duplicate, selection)) {
    setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
        "EditController", "The duplicate Grade Node could not be inserted safely")));
        return;
    }
    setGradeStack(std::move(updated), duplicate.grade_node_id);
    recordWorkingTransition(
        QStringLiteral("grade_node/%1/duplicate").arg(duplicate.grade_node_id),
        before
    );
    schedulePreview(0);
  setStatusMessage(edit_message(
      QT_TRANSLATE_NOOP("EditController", "Duplicated Grade Node · %1"),
      {display_grade_node_label(duplicate.label)}));
}

void EditController::refreshSharedGradeNodes() {
    try {
        const auto refreshed = backend_->sharedGradeNodes();
        if (refreshed != shared_grade_nodes_) {
            shared_grade_nodes_ = refreshed;
            emit sharedGradeNodesChanged();
            emit gradeNodesChanged();
        }
    } catch (const std::exception& error) {
        setStatusMessage(edit_message(
            QT_TRANSLATE_NOOP("EditController",
                              "Could not load shared Grade Nodes · %1"),
            {QString::fromUtf8(error.what())}
        ));
    }
}

void EditController::publishSelectedGradeNode(const QString& label) {
    const auto* const selected = selectedGradeNode();
    const QString normalized_label = label.trimmed();
    if (selected == nullptr || interactionLocked()) {
        return;
    }
    if (normalized_label.isEmpty()) {
        setStatusMessage(edit_message(
            QT_TRANSLATE_NOOP("EditController",
                              "Give the shared Grade Node a name")
        ));
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    const bool enabled = selected->enabled;
    BackendSharedGradeNode published;
    try {
        published = backend_->publishSharedGradeNode(normalized_label, *selected);
    } catch (const std::exception& error) {
        setStatusMessage(edit_message(
            QT_TRANSLATE_NOOP("EditController",
                              "Could not share Grade Node · %1"),
            {QString::fromUtf8(error.what())}
        ));
        return;
    }
    published.grade_node.enabled = enabled;
    // Sharing publishes only the adjustment graph. The current photo keeps
    // its own spatial placement for that Grade Node.
    published.grade_node.local_mask_kind = selected->local_mask_kind;
    published.grade_node.local_mask_x0 = selected->local_mask_x0;
    published.grade_node.local_mask_y0 = selected->local_mask_y0;
    published.grade_node.local_mask_x1 = selected->local_mask_x1;
    published.grade_node.local_mask_y1 = selected->local_mask_y1;
    published.grade_node.local_mask_radius_x = selected->local_mask_radius_x;
    published.grade_node.local_mask_radius_y = selected->local_mask_radius_y;
    published.grade_node.local_mask_feather = selected->local_mask_feather;
    published.grade_node.local_mask_invert = selected->local_mask_invert;
    published.grade_node.local_mask_brush_points =
        selected->local_mask_brush_points;
    BackendGradeStack updated = grade_stack_;
    updated.grade_nodes[selected_grade_node_index_] = published.grade_node;
    setGradeStack(std::move(updated), published.grade_node.grade_node_id);
    recordWorkingTransition(
        QStringLiteral("grade_node/%1/share/%2")
            .arg(published.grade_node.grade_node_id, published.revision_id),
        before
    );
    refreshSharedGradeNodes();
    schedulePreview(0);
    setStatusMessage(edit_message(
        QT_TRANSLATE_NOOP("EditController", "Shared Grade Node · %1"),
        {published.label}
    ));
}

void EditController::insertSharedGradeNode(const QString& layer_id) {
    const auto iterator = std::find_if(
        shared_grade_nodes_.cbegin(),
        shared_grade_nodes_.cend(),
        [&layer_id](const BackendSharedGradeNode& shared) {
            return shared.layer_id == layer_id;
        }
    );
    if (iterator == shared_grade_nodes_.cend() || !active_ || interactionLocked()) {
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    BackendGradeStack updated = grade_stack_;
    const auto existing = std::find_if(
        updated.grade_nodes.begin(),
        updated.grade_nodes.end(),
        [&iterator](const BackendGradeNode& node) {
            return node.shared_layer_id == iterator->layer_id
                || node.grade_node_id == iterator->grade_node.grade_node_id;
        }
    );
    BackendGradeNode inserted = iterator->grade_node;
    if (existing != updated.grade_nodes.end()) {
        inserted.enabled = existing->enabled;
        inserted.local_mask_kind = existing->local_mask_kind;
        inserted.local_mask_x0 = existing->local_mask_x0;
        inserted.local_mask_y0 = existing->local_mask_y0;
        inserted.local_mask_x1 = existing->local_mask_x1;
        inserted.local_mask_y1 = existing->local_mask_y1;
        inserted.local_mask_radius_x = existing->local_mask_radius_x;
        inserted.local_mask_radius_y = existing->local_mask_radius_y;
        inserted.local_mask_feather = existing->local_mask_feather;
        inserted.local_mask_invert = existing->local_mask_invert;
        inserted.local_mask_brush_points = existing->local_mask_brush_points;
        *existing = inserted;
    } else {
        if (!canAddGradeNode()) {
            setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
                "EditController", "An edit can contain at most 16 Grade Nodes")));
            return;
        }
        int selection = selected_grade_node_index_;
        if (!GradeNodeStack::insertAfterSelection(updated, inserted, selection)) {
            setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
                "EditController",
                "The shared Grade Node could not be inserted safely")));
            return;
        }
    }
    setGradeStack(std::move(updated), inserted.grade_node_id);
    recordWorkingTransition(
        QStringLiteral("grade_node/%1/shared/%2")
            .arg(inserted.grade_node_id, iterator->revision_id),
        before
    );
    schedulePreview(0);
    setStatusMessage(edit_message(
        QT_TRANSLATE_NOOP("EditController", "Applied shared Grade Node · %1"),
        {iterator->label}
    ));
}

void EditController::setSelectedLocalMask(const int kind) {
    auto* const grade_node = selected_grade_node_index_ < 0
        ? nullptr : &grade_stack_.grade_nodes[selected_grade_node_index_];
    if (!active_ || interactionLocked() || grade_node == nullptr || !grade_node->enabled
        || kind < 0 || kind > 3 || grade_node->local_mask_kind == kind) {
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    grade_node->local_mask_kind = static_cast<std::uint8_t>(kind);
    grade_node->local_mask_invert = false;
    grade_node->local_mask_brush_points.clear();
    if (kind == 0) {
        grade_node->local_mask_x0 = 0.0;
        grade_node->local_mask_y0 = 0.0;
        grade_node->local_mask_x1 = 0.0;
        grade_node->local_mask_y1 = 0.0;
        grade_node->local_mask_radius_x = 0.0;
        grade_node->local_mask_radius_y = 0.0;
        grade_node->local_mask_feather = 0.0;
    } else if (kind == 1) {
        grade_node->local_mask_x0 = 0.25;
        grade_node->local_mask_y0 = 0.5;
        grade_node->local_mask_x1 = 0.75;
        grade_node->local_mask_y1 = 0.5;
        grade_node->local_mask_radius_x = 0.0;
        grade_node->local_mask_radius_y = 0.0;
        grade_node->local_mask_feather = 0.0;
    } else if (kind == 2) {
        grade_node->local_mask_x0 = 0.5;
        grade_node->local_mask_y0 = 0.5;
        grade_node->local_mask_x1 = 0.0;
        grade_node->local_mask_y1 = 0.0;
        grade_node->local_mask_radius_x = 0.28;
        grade_node->local_mask_radius_y = 0.28;
        grade_node->local_mask_feather = 0.35;
    } else {
        grade_node->local_mask_x0 = 0.0;
        grade_node->local_mask_y0 = 0.0;
        grade_node->local_mask_x1 = 0.0;
        grade_node->local_mask_y1 = 0.0;
        grade_node->local_mask_radius_x = 0.035;
        grade_node->local_mask_radius_y = 0.0;
        grade_node->local_mask_feather = 0.6;
    }
    parameterEdited(QStringLiteral("local_mask/kind"), before);
    emit gradeNodesChanged();
    setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
        "EditController",
        kind == 0 ? "Removed local mask"
                  : kind == 1 ? "Added linear gradient mask"
                  : kind == 2 ? "Added radial gradient mask"
                              : "Added brush mask"
    )));
}

void EditController::copySelectedLocalMask() {
    const auto* const grade_node = selectedGradeNode();
    if (!active_ || interactionLocked() || grade_node == nullptr
        || grade_node->local_mask_kind == 0U) {
        setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
            "EditController", "Select a node mask to copy"
        )));
        return;
    }
    finishActiveGesture();
    node_mask_clipboard_ = NodeMaskClipboard{
        .kind = grade_node->local_mask_kind,
        .x0 = grade_node->local_mask_x0,
        .y0 = grade_node->local_mask_y0,
        .x1 = grade_node->local_mask_x1,
        .y1 = grade_node->local_mask_y1,
        .radius_x = grade_node->local_mask_radius_x,
        .radius_y = grade_node->local_mask_radius_y,
        .feather = grade_node->local_mask_feather,
        .inverted = grade_node->local_mask_invert,
        .brush_points = grade_node->local_mask_brush_points,
    };
    emit nodeMaskClipboardChanged();
    setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
        "EditController", "Copied node mask"
    )));
}

void EditController::pasteSelectedLocalMask() {
    if (!node_mask_clipboard_.has_value()) {
        setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
            "EditController", "Copy a node mask before pasting"
        )));
        return;
    }
    auto* const grade_node = selected_grade_node_index_ < 0
        ? nullptr : &grade_stack_.grade_nodes[selected_grade_node_index_];
    if (!active_ || interactionLocked() || grade_node == nullptr || !grade_node->enabled) {
        setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
            "EditController", "Select an enabled Grade Node to paste the mask"
        )));
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    BackendGradeNode candidate = *grade_node;
    const NodeMaskClipboard& source = *node_mask_clipboard_;
    candidate.local_mask_kind = source.kind;
    candidate.local_mask_x0 = source.x0;
    candidate.local_mask_y0 = source.y0;
    candidate.local_mask_x1 = source.x1;
    candidate.local_mask_y1 = source.y1;
    candidate.local_mask_radius_x = source.radius_x;
    candidate.local_mask_radius_y = source.radius_y;
    candidate.local_mask_feather = source.feather;
    candidate.local_mask_invert = source.inverted;
    candidate.local_mask_brush_points = source.brush_points;
    if (candidate == *grade_node) {
        setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
            "EditController", "Selected Grade Node already uses the copied mask"
        )));
        return;
    }
    *grade_node = std::move(candidate);
    parameterEdited(QStringLiteral("local_mask/paste"), before);
    emit gradeNodesChanged();
    setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
        "EditController", "Pasted node mask"
    )));
}

void EditController::saveSelectedLocalMaskAsset(const QString& name) {
    const QString normalized_name = name.trimmed();
    const auto* const grade_node = selectedGradeNode();
    if (!active_ || interactionLocked() || grade_node == nullptr
        || grade_node->local_mask_kind == 0U) {
        setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
            "EditController", "Select a node mask to save as an asset"
        )));
        return;
    }
    if (normalized_name.isEmpty()) {
        setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
            "EditController", "Enter a mask asset name"
        )));
        return;
    }
    if (normalized_name.size() > MAX_NODE_MASK_ASSET_NAME_LENGTH) {
        setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
            "EditController", "Mask asset names can contain at most %1 characters"
        ), {MAX_NODE_MASK_ASSET_NAME_LENGTH}));
        return;
    }

    finishActiveGesture();
    QString id;
    qsizetype matching_index = -1;
    for (qsizetype index = 0; index < node_mask_assets_.size(); ++index) {
        const QVariantMap existing = node_mask_assets_.at(index).toMap();
        if (existing.value(QStringLiteral("name")).toString().compare(
                normalized_name,
                Qt::CaseInsensitive
            ) == 0) {
            id = existing.value(QStringLiteral("id")).toString();
            matching_index = index;
            break;
        }
    }
    if (matching_index < 0 && node_mask_assets_.size() >= MAX_NODE_MASK_ASSET_COUNT) {
        setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
            "EditController", "Mask asset library is full"
        )));
        return;
    }
    if (id.isEmpty()) {
        id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }

    const auto asset = normalized_node_mask_asset(
        node_mask_asset_from_node(id, normalized_name, *grade_node)
    );
    if (!asset.has_value()) {
        setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
            "EditController", "Current node mask cannot be saved as an asset"
        )));
        return;
    }
    if (matching_index >= 0) {
        node_mask_assets_[matching_index] = *asset;
    } else {
        node_mask_assets_.push_back(*asset);
    }
    std::sort(
        node_mask_assets_.begin(),
        node_mask_assets_.end(),
        [](const QVariant& left, const QVariant& right) {
            return left.toMap().value(QStringLiteral("name")).toString().compare(
                right.toMap().value(QStringLiteral("name")).toString(),
                Qt::CaseInsensitive
            ) < 0;
        }
    );
    persistNodeMaskAssets();
    emit nodeMaskAssetsChanged();
    setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
        "EditController", "Saved node mask asset · %1"
    ), {normalized_name}));
}

void EditController::applySelectedLocalMaskAsset(const QString& asset_id) {
    auto* const grade_node = selected_grade_node_index_ < 0
        ? nullptr : &grade_stack_.grade_nodes[selected_grade_node_index_];
    if (!active_ || interactionLocked() || grade_node == nullptr || !grade_node->enabled) {
        setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
            "EditController", "Select an enabled Grade Node to apply a mask asset"
        )));
        return;
    }
    const auto found = std::find_if(
        node_mask_assets_.cbegin(),
        node_mask_assets_.cend(),
        [&asset_id](const QVariant& value) {
            return value.toMap().value(QStringLiteral("id")).toString() == asset_id;
        }
    );
    if (found == node_mask_assets_.cend()) {
        setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
            "EditController", "Choose a saved mask asset to apply"
        )));
        return;
    }
    const QVariantMap asset = found->toMap();
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    BackendGradeNode candidate = *grade_node;
    candidate.local_mask_kind = static_cast<std::uint8_t>(
        asset.value(QStringLiteral("kind")).toInt()
    );
    candidate.local_mask_x0 = asset.value(QStringLiteral("x0")).toDouble();
    candidate.local_mask_y0 = asset.value(QStringLiteral("y0")).toDouble();
    candidate.local_mask_x1 = asset.value(QStringLiteral("x1")).toDouble();
    candidate.local_mask_y1 = asset.value(QStringLiteral("y1")).toDouble();
    candidate.local_mask_radius_x = asset.value(QStringLiteral("radiusX")).toDouble();
    candidate.local_mask_radius_y = asset.value(QStringLiteral("radiusY")).toDouble();
    candidate.local_mask_feather = asset.value(QStringLiteral("feather")).toDouble();
    candidate.local_mask_invert = asset.value(QStringLiteral("inverted")).toBool();
    candidate.local_mask_brush_points.clear();
    const QVariantList brush_points = asset.value(QStringLiteral("brushPoints")).toList();
    candidate.local_mask_brush_points.reserve(brush_points.size());
    for (const QVariant& value : brush_points) {
        candidate.local_mask_brush_points.push_back(value.toDouble());
    }
    if (candidate == *grade_node) {
        setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
            "EditController", "Selected Grade Node already uses this mask asset"
        )));
        return;
    }
    *grade_node = std::move(candidate);
    parameterEdited(QStringLiteral("local_mask/apply_asset"), before);
    emit gradeNodesChanged();
    setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
        "EditController", "Applied node mask asset · %1"
    ), {asset.value(QStringLiteral("name")).toString()}));
}

void EditController::removeLocalMaskAsset(const QString& asset_id) {
    const auto found = std::find_if(
        node_mask_assets_.cbegin(),
        node_mask_assets_.cend(),
        [&asset_id](const QVariant& value) {
            return value.toMap().value(QStringLiteral("id")).toString() == asset_id;
        }
    );
    if (found == node_mask_assets_.cend()) {
        return;
    }
    const QString name = found->toMap().value(QStringLiteral("name")).toString();
    node_mask_assets_.erase(found);
    persistNodeMaskAssets();
    emit nodeMaskAssetsChanged();
    setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
        "EditController", "Removed node mask asset · %1"
    ), {name}));
}

void EditController::setSelectedLocalMaskValue(
    const QString& key,
    const double value
) {
    auto* const grade_node = selected_grade_node_index_ < 0
        ? nullptr : &grade_stack_.grade_nodes[selected_grade_node_index_];
    if (grade_node == nullptr || grade_node->local_mask_kind == 0U
        || !acceptParameter(
            value,
            0.0,
            1.0,
            QT_TRANSLATE_NOOP("EditController", "Local mask")
        )) {
        return;
    }

    double* target = nullptr;
    if (key == QStringLiteral("x0")) target = &grade_node->local_mask_x0;
    else if (key == QStringLiteral("y0")) target = &grade_node->local_mask_y0;
    else if (key == QStringLiteral("x1")) target = &grade_node->local_mask_x1;
    else if (key == QStringLiteral("y1")) target = &grade_node->local_mask_y1;
    else if (key == QStringLiteral("radiusX")) target = &grade_node->local_mask_radius_x;
    else if (key == QStringLiteral("radiusY")) target = &grade_node->local_mask_radius_y;
    else if (key == QStringLiteral("feather")) target = &grade_node->local_mask_feather;
    if (target == nullptr || *target == value) {
        return;
    }

    BackendGradeNode candidate = *grade_node;
    if (key == QStringLiteral("x0")) candidate.local_mask_x0 = value;
    else if (key == QStringLiteral("y0")) candidate.local_mask_y0 = value;
    else if (key == QStringLiteral("x1")) candidate.local_mask_x1 = value;
    else if (key == QStringLiteral("y1")) candidate.local_mask_y1 = value;
    else if (key == QStringLiteral("radiusX")) candidate.local_mask_radius_x = value;
    else if (key == QStringLiteral("radiusY")) candidate.local_mask_radius_y = value;
    else candidate.local_mask_feather = value;
    if (candidate.local_mask_kind == 1U
        && std::hypot(
            candidate.local_mask_x1 - candidate.local_mask_x0,
            candidate.local_mask_y1 - candidate.local_mask_y0
        ) < 0.01) {
        setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
            "EditController", "A gradient mask needs two distinct points"
        )));
        return;
    }
    if (candidate.local_mask_kind == 2U
        && (candidate.local_mask_radius_x < 0.01
            || candidate.local_mask_radius_y < 0.01)) {
        setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
            "EditController", "A radial mask needs a non-zero radius"
        )));
        return;
    }
    if (candidate.local_mask_kind == 3U
        && candidate.local_mask_radius_x < 0.005) {
        setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
            "EditController", "A brush mask needs a non-zero size"
        )));
        return;
    }

    const BackendGradeStack before = grade_stack_;
    *grade_node = std::move(candidate);
    parameterEdited(QStringLiteral("local_mask/%1").arg(key), before);
    emit gradeNodesChanged();
}

void EditController::setSelectedLocalMaskPoint(
    const QString& point,
    const double normalized_x,
    const double normalized_y
) {
    auto* const grade_node = selected_grade_node_index_ < 0
        ? nullptr : &grade_stack_.grade_nodes[selected_grade_node_index_];
    if (grade_node == nullptr || grade_node->local_mask_kind == 0U
        || !acceptParameter(
            normalized_x,
            0.0,
            1.0,
            QT_TRANSLATE_NOOP("EditController", "Local mask")
        )
        || !acceptParameter(
            normalized_y,
            0.0,
            1.0,
            QT_TRANSLATE_NOOP("EditController", "Local mask")
        )) {
        return;
    }

    BackendGradeNode candidate = *grade_node;
    if (point == QStringLiteral("start") && candidate.local_mask_kind == 1U) {
        candidate.local_mask_x0 = normalized_x;
        candidate.local_mask_y0 = normalized_y;
    } else if (point == QStringLiteral("end")
               && candidate.local_mask_kind == 1U) {
        candidate.local_mask_x1 = normalized_x;
        candidate.local_mask_y1 = normalized_y;
    } else if (point == QStringLiteral("center")
               && candidate.local_mask_kind == 2U) {
        candidate.local_mask_x0 = normalized_x;
        candidate.local_mask_y0 = normalized_y;
    } else {
        return;
    }

    if (candidate == *grade_node) {
        return;
    }
    if (candidate.local_mask_kind == 1U
        && std::hypot(
            candidate.local_mask_x1 - candidate.local_mask_x0,
            candidate.local_mask_y1 - candidate.local_mask_y0
        ) < 0.01) {
        setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
            "EditController", "A gradient mask needs two distinct points"
        )));
        return;
    }

    const BackendGradeStack before = grade_stack_;
    *grade_node = std::move(candidate);
    parameterEdited(QStringLiteral("local_mask/%1").arg(point), before);
    emit gradeNodesChanged();
}

void EditController::setSelectedLocalMaskInverted(const bool inverted) {
    auto* const grade_node = selected_grade_node_index_ < 0
        ? nullptr : &grade_stack_.grade_nodes[selected_grade_node_index_];
    if (!active_ || interactionLocked() || grade_node == nullptr || !grade_node->enabled
        || grade_node->local_mask_kind == 0U
        || grade_node->local_mask_invert == inverted) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    grade_node->local_mask_invert = inverted;
    parameterEdited(QStringLiteral("local_mask/invert"), before);
    emit gradeNodesChanged();
}

void EditController::appendSelectedLocalMaskBrushPoint(
    const double normalized_x,
    const double normalized_y,
    const bool begins_stroke
) {
    constexpr qsizetype maximum_brush_points = 4'096;
    constexpr double minimum_point_distance = 0.0015;
    auto* const grade_node = selected_grade_node_index_ < 0
        ? nullptr : &grade_stack_.grade_nodes[selected_grade_node_index_];
    if (grade_node == nullptr || grade_node->local_mask_kind != 3U
        || !acceptParameter(
            normalized_x,
            0.0,
            1.0,
            QT_TRANSLATE_NOOP("EditController", "Brush mask")
        )
        || !acceptParameter(
            normalized_y,
            0.0,
            1.0,
            QT_TRANSLATE_NOOP("EditController", "Brush mask")
        )) {
        return;
    }
    const qsizetype point_count = grade_node->local_mask_brush_points.size() / 3;
    if (point_count >= maximum_brush_points) {
        setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
            "EditController", "This brush mask has reached its point limit"
        )));
        return;
    }
    if (!begins_stroke && point_count > 0) {
        const qsizetype previous = grade_node->local_mask_brush_points.size() - 3;
        if (std::hypot(
                normalized_x - grade_node->local_mask_brush_points.at(previous),
                normalized_y - grade_node->local_mask_brush_points.at(previous + 1)
            ) < minimum_point_distance) {
            return;
        }
    }
    const BackendGradeStack before = grade_stack_;
    grade_node->local_mask_brush_points.push_back(normalized_x);
    grade_node->local_mask_brush_points.push_back(normalized_y);
    grade_node->local_mask_brush_points.push_back(begins_stroke ? 1.0 : 0.0);
    parameterEdited(QStringLiteral("local_mask/brush"), before);
    emit gradeNodesChanged();
}

void EditController::clearSelectedLocalMaskBrush() {
    auto* const grade_node = selected_grade_node_index_ < 0
        ? nullptr : &grade_stack_.grade_nodes[selected_grade_node_index_];
    if (!active_ || interactionLocked() || grade_node == nullptr
        || grade_node->local_mask_kind != 3U
        || grade_node->local_mask_brush_points.isEmpty()) {
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    grade_node->local_mask_brush_points.clear();
    parameterEdited(QStringLiteral("local_mask/brush/clear"), before);
    emit gradeNodesChanged();
}

void EditController::deleteSelectedGradeNode() {
    const auto* const selected = selectedGradeNode();
    if (!canDeleteGradeNode() || selected == nullptr) {
        return;
    }
    finishActiveGesture();
    const QString deleted_id = selected->grade_node_id;
    const QString deleted_label = selected->label;
    const BackendGradeStack before = grade_stack_;
    BackendGradeStack updated = grade_stack_;
    int selection = selected_grade_node_index_;
    if (!GradeNodeStack::deleteSelection(updated, selection)) {
        return;
    }
    const QString next_id = updated.grade_nodes.at(selection).grade_node_id;
    setGradeStack(std::move(updated), next_id);
    recordWorkingTransition(
        QStringLiteral("grade_node/%1/delete").arg(deleted_id),
        before
    );
    schedulePreview(0);
  setStatusMessage(edit_message(
      QT_TRANSLATE_NOOP("EditController", "Deleted Grade Node · %1"),
      {display_grade_node_label(deleted_label)}));
}

void EditController::moveSelectedGradeNode(const int destination_index) {
    const auto* const selected = selectedGradeNode();
    if (!active_ || interactionLocked() || selected == nullptr) {
        return;
    }
    finishActiveGesture();
    const QString moved_id = selected->grade_node_id;
    const BackendGradeStack before = grade_stack_;
    BackendGradeStack updated = grade_stack_;
    int selection = selected_grade_node_index_;
    if (!GradeNodeStack::moveSelection(updated, selection, destination_index)) {
        return;
    }
    setGradeStack(std::move(updated), moved_id);
    recordWorkingTransition(
        QStringLiteral("grade_node/%1/move").arg(moved_id),
        before
    );
    schedulePreview(0);
  setStatusMessage(edit_message(
      QT_TRANSLATE_NOOP("EditController", "Reordered Grade Node")));
}

void EditController::beginParameterEdit(const QString& parameter_key) {
    const auto* const grade_node = selectedGradeNode();
    const bool photo_local_retouch = parameter_key.startsWith(QStringLiteral("retouch/"));
    const bool photo_local_geometry = parameter_key.startsWith(QStringLiteral("geometry/"));
    if (!active_ || interactionLocked()
        || (!photo_local_retouch && !photo_local_geometry
            && (grade_node == nullptr || !grade_node->enabled))
        || parameter_key.isEmpty()) {
        return;
    }
    const bool could_undo = canUndo();
    const bool could_redo = canRedo();
    if (active_parameter_gestures_.isEmpty()) {
        first_interactive_frame_presented_ = false;
    }
    active_parameter_gestures_.insert(parameter_key);
    history_.beginGesture(gradeNodeHistoryKey(parameter_key).toStdString(), grade_stack_);
    if (could_undo != canUndo() || could_redo != canRedo()) {
        emit historyChanged();
    }
}

void EditController::endParameterEdit(const QString& parameter_key) {
    if (!active_ || parameter_key.isEmpty()) {
        return;
    }
    const bool could_undo = canUndo();
    const bool could_redo = canRedo();
    history_.endGesture(gradeNodeHistoryKey(parameter_key).toStdString(), grade_stack_);
    const bool ended_active_gesture = active_parameter_gestures_.remove(parameter_key) > 0;
    if (could_undo != canUndo() || could_redo != canRedo()) {
        emit historyChanged();
    }
    if (ended_active_gesture && active_parameter_gestures_.isEmpty()) {
        // Replace the low-latency gesture proxy with a normal-resolution
        // frame for the exact final slider value.
        first_interactive_frame_presented_ = false;
        cancelActivePreview(true);
        schedulePreview(0);
    }
}

void EditController::beginToneCurveGesture(const int index) {
    const auto* const grade_node = selectedGradeNode();
    if (!active_ || interactionLocked() || grade_node == nullptr || !grade_node->enabled
        || !tone_curve_points_.isEditable()
        || !tone_curve_points_.selectPoint(index)) {
        return;
    }
    beginParameterEdit(tone_curve_gesture_key(index));
}

void EditController::moveToneCurvePoint(
    const int index,
    const double x,
    const double y
) {
    const auto* const grade_node = selectedGradeNode();
    if (!active_ || interactionLocked() || grade_node == nullptr || !grade_node->enabled) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    if (!tone_curve_points_.movePoint(index, x, y)) {
        return;
    }
    auto& edited = grade_stack_.grade_nodes[selected_grade_node_index_];
    ensure_oklab_lightness_curve(edited.fine);
    edited.fine.oklab_lightness_curve_points =
        backend_oklab_lightness_curve_points(tone_curve_points_);
    toneCurveEdited(
        tone_curve_gesture_key(index),
        before,
        EDIT_PREVIEW_THROTTLE_MS
    );
}

void EditController::endToneCurveGesture(const int index) {
    endParameterEdit(tone_curve_gesture_key(index));
}

void EditController::addToneCurvePoint(const double x, const double y) {
    const auto* const grade_node = selectedGradeNode();
    if (!active_ || interactionLocked() || grade_node == nullptr || !grade_node->enabled) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    if (tone_curve_points_.addPoint(x, y) < 0) {
    setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
        "EditController", "The point cannot be added inside this curve")));
        return;
    }
    auto& edited = grade_stack_.grade_nodes[selected_grade_node_index_];
    ensure_oklab_lightness_curve(edited.fine);
    edited.fine.oklab_lightness_curve_points =
        backend_oklab_lightness_curve_points(tone_curve_points_);
    toneCurveEdited(
        QStringLiteral("perceptual_tone_curve/add"),
        before,
        0
    );
}

void EditController::removeToneCurvePoint(const int index) {
    const auto* const grade_node = selectedGradeNode();
    if (!active_ || interactionLocked() || grade_node == nullptr || !grade_node->enabled) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    if (!tone_curve_points_.removePoint(index)) {
        return;
    }
    auto& edited = grade_stack_.grade_nodes[selected_grade_node_index_];
    ensure_oklab_lightness_curve(edited.fine);
    edited.fine.oklab_lightness_curve_points =
        backend_oklab_lightness_curve_points(tone_curve_points_);
    clear_neutral_oklab_lightness_curve(edited.fine);
    toneCurveEdited(
        QStringLiteral("perceptual_tone_curve/remove"),
        before,
        0
    );
}

void EditController::resetToneCurve() {
    const auto* const grade_node = selectedGradeNode();
    if (!active_ || interactionLocked() || grade_node == nullptr || !grade_node->enabled
        || !hasToneCurve()) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    tone_curve_points_.resetLinear();
    auto& edited = grade_stack_.grade_nodes[selected_grade_node_index_];
    edited.fine.oklab_lightness_curve_points.clear();
    toneCurveEdited(
        QStringLiteral("perceptual_tone_curve/reset"),
        before,
        0
    );
}

void EditController::undo() {
    if (!active_ || interactionLocked()) {
        return;
    }
    std::string history_key;
    const auto restored = history_.undo(grade_stack_, &history_key);
    emit historyChanged();
    if (!restored) {
        return;
    }
    QString preferred_id = history_grade_node_id(history_key);
    const bool undoes_insert = history_key.ends_with("/add")
        || history_key.ends_with("/duplicate");
    if (undoes_insert
        && GradeNodeStack::gradeNodeIndex(*restored, preferred_id) < 0
        && !restored->grade_nodes.isEmpty()) {
        const int previous_index = std::clamp(
            selected_grade_node_index_ - 1,
            0,
            static_cast<int>(restored->grade_nodes.size() - 1)
        );
        preferred_id = restored->grade_nodes.at(previous_index).grade_node_id;
    }
    autosave_requested_ = true;
    clearAutosaveFailure();
    ++working_revision_;
    setGradeStack(*restored, preferred_id);
    schedulePreview(0);
  setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
      "EditController", "Undid the last session adjustment")));
}

void EditController::redo() {
    if (!active_ || interactionLocked()) {
        return;
    }
    std::string history_key;
    const auto restored = history_.redo(grade_stack_, &history_key);
    emit historyChanged();
    if (!restored) {
        return;
    }
    autosave_requested_ = true;
    clearAutosaveFailure();
    ++working_revision_;
    setGradeStack(*restored, history_grade_node_id(history_key));
    schedulePreview(0);
  setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
      "EditController", "Redid the last session adjustment")));
}

void EditController::resetSelectedGradeNode() {
    const auto* const grade_node = selectedGradeNode();
    if (!active_ || interactionLocked() || grade_node == nullptr) {
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    BackendGradeStack reset = grade_stack_;
    if (!GradeNodeStack::resetSelection(reset, selected_grade_node_index_)) {
        return;
    }
    const QString grade_node_id = grade_node->grade_node_id;
    setGradeStack(std::move(reset), grade_node_id);
    recordWorkingTransition(
        QStringLiteral("grade_node/%1/reset").arg(grade_node_id),
        before
    );
    schedulePreview(0);
    setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
      "EditController", "Reset the selected Grade Node")));
}

void EditController::resetAllGradeNodes() {
    if (!active_ || interactionLocked()) {
        return;
    }
    finishActiveGesture();

    BackendGradeNode neutral;
    try {
        neutral = backend_->newBasicGradeNode(QStringLiteral("Adjustments"));
    } catch (const std::exception& error) {
        setStatusMessage(edit_message(
            QT_TRANSLATE_NOOP(
                "EditController",
                "Could not clear Grade Nodes · %1"
            ),
            {QString::fromUtf8(error.what())}
        ));
        return;
    }

    const BackendGradeStack before = grade_stack_;
    BackendGradeStack reset = grade_stack_;
    reset.grade_nodes = {neutral};
    setGradeStack(std::move(reset), neutral.grade_node_id);
    recordWorkingTransition(QStringLiteral("grade_nodes/reset_all"), before);
    schedulePreview(0);
    setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
        "EditController",
        "Cleared all Grade Nodes"
    )));
}

void EditController::resetAllAdjustments() {
    if (!active_ || interactionLocked()) {
        return;
    }
    finishActiveGesture();

    BackendGradeNode neutral;
    try {
        neutral = backend_->newBasicGradeNode(QStringLiteral("Adjustments"));
    } catch (const std::exception& error) {
        setStatusMessage(edit_message(
            QT_TRANSLATE_NOOP(
                "EditController",
                "Could not reset adjustments · %1"
            ),
            {QString::fromUtf8(error.what())}
        ));
        return;
    }

    const BackendGradeStack before = grade_stack_;
    BackendGradeStack reset;
    reset.grade_nodes = {neutral};
    setFullResolutionState(false, false, 0);
    setGradeStack(std::move(reset), neutral.grade_node_id);
    recordWorkingTransition(QStringLiteral("adjustments/reset_all"), before);
    schedulePreview(0);
    setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
        "EditController",
        "Reset all adjustments"
    )));
}

void EditController::revertEdits() {
    if (!active_ || state_running_) {
        return;
    }
    if (version_draft_) {
        setVersionDraft(false);
        base_commit_id_ = durable_working_commit_id_;
        versions_.setSelectedCommit(durable_working_commit_id_);
        setGradeStack(committed_grade_stack_);
        clearSessionHistory();
        schedulePreview(0);
        setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
            "EditController", "Discarded working changes and restored the Library version")));
        return;
    }
    if (grade_stack_ != committed_grade_stack_) {
        const BackendGradeStack before = grade_stack_;
        setGradeStack(committed_grade_stack_);
        recordWorkingTransition(QStringLiteral("revert"), before);
        schedulePreview(0);
    }
    autosave_debounce_.stop();
    clearAutosaveFailure();
    if (autosave_requested_) {
        autosave_requested_ = false;
        emit autosavePendingChanged();
    }
  setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
      "EditController", "Restored the current saved version")));
}

void EditController::setGradeStack(
    BackendGradeStack grade_stack,
    const QString& preferred_grade_node_id
) {
    if (grade_stack.grade_nodes.size() > GradeNodeStack::maximum_grade_node_count) {
    setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
        "EditController",
        "The saved edit exceeds the 16-Grade-Node desktop limit")));
        return;
    }
    const auto old_optics = grade_stack_.optics;
    const QString old_selected_id = selectedGradeNodeId();
    const int old_selected_index = selected_grade_node_index_;
    const BackendGradeNode* const old_selected = selectedGradeNode();
    const bool had_old_selection = old_selected != nullptr;
    const BackendGradeNode old_selected_value = had_old_selection
        ? *old_selected
        : BackendGradeNode{};
    const QString requested_id = preferred_grade_node_id.isEmpty()
        ? old_selected_id
        : preferred_grade_node_id;
    const int new_selected_index = GradeNodeStack::resolvedSelection(
        grade_stack,
        requested_id,
        old_selected_index
    );
    const BackendGradeNode* const new_selected = new_selected_index < 0
        ? nullptr
        : &grade_stack.grade_nodes.at(new_selected_index);
    const bool has_new_selection = new_selected != nullptr;
    const bool selection_changed = old_selected_index != new_selected_index
        || old_selected_id
            != (has_new_selection ? new_selected->grade_node_id : QString{});
    const bool grade_node_enabled_changed = selection_changed
        || had_old_selection != has_new_selection
        || (had_old_selection && has_new_selection
            && old_selected_value.enabled != new_selected->enabled);
    const bool basic_changed = selection_changed
        || had_old_selection != has_new_selection
        || (had_old_selection && has_new_selection
            && (old_selected_value.basic != new_selected->basic
                || old_selected_value.fine != new_selected->fine));
    const bool retouch_changed = grade_stack_.retouch_spots != grade_stack.retouch_spots
        || grade_stack_.retouch_strokes != grade_stack.retouch_strokes;
    const bool curve_changed = selection_changed
        || had_old_selection != has_new_selection
        || (had_old_selection && has_new_selection
            && old_selected_value.fine.oklab_lightness_curve_points
                != new_selected->fine.oklab_lightness_curve_points);
    const bool list_changed = grade_node_list_changed(grade_stack_, grade_stack);
    const auto model_points = tone_curve_model_points(new_selected);
    if (tone_curve_points_.points() != model_points
        && !tone_curve_points_.replace(model_points)) {
    setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
        "EditController",
        "The saved Tone Curve cannot be represented safely")));
        return;
    }
    grade_stack_ = std::move(grade_stack);
    active_retouch_stroke_index_ = -1;
    selected_grade_node_index_ = new_selected_index;
    const int new_point_color_count = selectedGradeNode() == nullptr
        ? 0 : point_color_count(selectedGradeNode()->fine);
    selected_point_color_index_ = new_point_color_count == 0
        ? -1
        : selection_changed ? 0
                            : std::clamp(selected_point_color_index_, 0, new_point_color_count - 1);
    if (selection_changed || new_point_color_count == 0) {
        clearPointColorScopeReference();
    }
    if (list_changed) {
        emit gradeNodesChanged();
    }
    if (selection_changed) {
        emit selectedGradeNodeChanged();
    }
    if (list_changed || selection_changed) {
        emit gradeNodeActionsChanged();
    }
    if (grade_node_enabled_changed) {
        emit gradeNodeEnabledChanged();
    }
    if (basic_changed || retouch_changed) {
        notifyParametersChanged();
    }
    if (curve_changed) {
        emit toneCurveChanged();
    }
    if (old_optics != grade_stack_.optics) {
        emit opticsChanged();
    }
    setDirty(version_draft_ || grade_stack_ != committed_grade_stack_);
}

const BackendGradeNode* EditController::selectedGradeNode() const noexcept {
    const int count = static_cast<int>(grade_stack_.grade_nodes.size());
    if (selected_grade_node_index_ < 0 || selected_grade_node_index_ >= count) {
        return nullptr;
    }
    return &grade_stack_.grade_nodes.at(selected_grade_node_index_);
}

QString EditController::gradeNodeHistoryKey(const QString& key) const {
    if (key.startsWith(QStringLiteral("retouch/"))
        || key.startsWith(QStringLiteral("geometry/"))) {
        return QStringLiteral("photo/%1").arg(key);
    }
    const auto* const grade_node = selectedGradeNode();
    return grade_node == nullptr
        ? key
        : QStringLiteral("grade_node/%1/%2").arg(grade_node->grade_node_id, key);
}

QString EditController::uniqueGradeNodeLabel(const QString& base) const {
    const QString clean_base = base.trimmed().isEmpty()
        ? QStringLiteral("Adjustments")
        : base.trimmed();
    const auto exists = [this](const QString& candidate) {
        return std::any_of(
            grade_stack_.grade_nodes.cbegin(),
            grade_stack_.grade_nodes.cend(),
            [&candidate](const BackendGradeNode& grade_node) {
                return grade_node.label == candidate;
            }
        );
    };
    if (!exists(clean_base)) {
        return clean_base;
    }
    for (int suffix = 2; suffix <= GradeNodeStack::maximum_grade_node_count + 1; ++suffix) {
        const QString candidate = QStringLiteral("%1 %2").arg(clean_base).arg(suffix);
        if (!exists(candidate)) {
            return candidate;
        }
    }
    return clean_base + QStringLiteral(" Copy");
}

void EditController::finishActiveGesture() {
    active_parameter_gestures_.clear();
    first_interactive_frame_presented_ = false;
    cancelActivePreview(true);
    const bool could_undo = canUndo();
    const bool could_redo = canRedo();
    history_.finishGesture(grade_stack_);
    if (could_undo != canUndo() || could_redo != canRedo()) {
        emit historyChanged();
    }
}

void EditController::clearSessionHistory() {
    const bool had_history = history_.canUndo() || history_.canRedo();
    history_.clear();
    if (had_history) {
        emit historyChanged();
    }
}

void EditController::recordWorkingTransition(
    const QString& key,
    const BackendGradeStack& before
) {
    const bool could_undo = canUndo();
    const bool could_redo = canRedo();
    history_.record(key.toStdString(), before, grade_stack_);
    if (could_undo != canUndo() || could_redo != canRedo()) {
        emit historyChanged();
    }
    ++working_revision_;
    autosave_requested_ = true;
    clearAutosaveFailure();
    if (dirty_ && !state_running_) {
        scheduleAutosave();
    }
}

bool EditController::eventFilter(QObject *const watched, QEvent *const event) {
  if (watched == QCoreApplication::instance() &&
      event->type() == QEvent::LanguageChange) {
    retranslateUi();
  }
  return QObject::eventFilter(watched, event);
}

void EditController::retranslateUi() {
  emit statusTextChanged();
  if (!autosave_error_message_.isEmpty()) {
    emit autosaveErrorTextChanged();
  }
  emit gradeNodesChanged();
  if (!before_error_message_.isEmpty()) {
    emit beforeErrorTextChanged();
  }
  if (!detail_error_message_.isEmpty()) {
    emit detailErrorTextChanged();
  }
}

void EditController::setStatusMessage(LocalizedUiMessage status) {
    if (status_message_ == status) {
        return;
    }
  status_message_ = std::move(status);
    emit statusTextChanged();
}

void EditController::parameterEdited(
    const QString& key,
    const BackendGradeStack& before
) {
    if (!active_ || interactionLocked()) {
        return;
    }
    recordWorkingTransition(gradeNodeHistoryKey(key), before);
    notifyParametersChanged();
    setDirty(version_draft_ || grade_stack_ != committed_grade_stack_);
    schedulePreview(EDIT_PREVIEW_THROTTLE_MS);
}

void EditController::opticsEdited(
    const QString& key,
    const BackendGradeStack& before
) {
    recordWorkingTransition(QStringLiteral("optics/%1").arg(key), before);
    setFullResolutionState(false, false, 0);
    emit opticsChanged();
    setDirty(version_draft_ || grade_stack_ != committed_grade_stack_);
    // Geometry remapping is materially more expensive than a scalar Grade
    // adjustment. Coalesce slider samples to one interactive frame instead
    // of scheduling a separate complete render for every mouse move.
    schedulePreview(EDIT_PREVIEW_THROTTLE_MS);
}

void EditController::notifyParametersChanged() {
    if (point_color_scope_active_ && !pointColorScopeAvailable()) {
        point_color_scope_active_ = false;
        refreshCurrentDisplayScope();
        emit pointColorScopeChanged();
    } else if (point_color_scope_active_) {
        // Selecting another Point Color or changing its hue interval should
        // update the diagnostic immediately. The rendered preview stays
        // untouched; its normal async replacement is still scheduled by the
        // edit mutation that reached this notification.
        refreshCurrentDisplayScope();
    }
    ++parameter_revision_;
    emit parametersChanged();
}

void EditController::toneCurveEdited(
    const QString& key,
    const BackendGradeStack& before,
    const int preview_delay_ms
) {
    recordWorkingTransition(gradeNodeHistoryKey(key), before);
    emit toneCurveChanged();
    setDirty(version_draft_ || grade_stack_ != committed_grade_stack_);
    schedulePreview(preview_delay_ms);
}

bool EditController::acceptParameter(
    const double value,
    const double minimum,
    const double maximum,
    const char *const label_source) {
    if (!active_ || interactionLocked()) {
        return false;
    }
    const auto* const grade_node = selectedGradeNode();
    if (grade_node == nullptr) {
    setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
        "EditController", "Select a Grade Node before editing")));
        return false;
    }
    if (!grade_node->enabled) {
    setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
        "EditController",
        "Enable the selected Grade Node before editing its controls")));
        return false;
    }
    if (!std::isfinite(value) || value < minimum || value > maximum) {
    setStatusMessage(edit_message(
        QT_TRANSLATE_NOOP("EditController",
                          "%1 is outside the supported preview range"),
        {LocalizedUiArgument::translatedText("EditController", label_source)})
        );
        return false;
    }
    return true;
}
