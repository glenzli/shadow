#include "edit_controller.hpp"

#include "edit_stack.hpp"

#include <QtConcurrent>

#include <QSize>
#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {

constexpr std::uint32_t EDIT_PREVIEW_EDGE = 1'600;
constexpr std::uint8_t EDIT_PREVIEW_QUALITY = 88;
constexpr int EDIT_DEBOUNCE_MS = 140;
constexpr std::uint32_t EDIT_DETAIL_TILE_SIDE = 512;
constexpr std::uint32_t EDIT_LARGE_DETAIL_TILE_SIDE = 1'024;
constexpr int EDIT_DETAIL_DEBOUNCE_MS = 70;
constexpr std::uint64_t EDIT_DETAIL_MAX_PRESENTATION_BYTES = 96U * 1'024U * 1'024U;

[[nodiscard]] QVector<ToneCurvePoint> tone_curve_model_points(
    const BackendBasicEditLayer* const layer
) {
    if (layer == nullptr || !layer->has_tone_curve) {
        return {{0.0, 0.0}, {1.0, 1.0}};
    }
    QVector<ToneCurvePoint> points;
    points.reserve(layer->tone_curve_points.size());
    for (const auto& point : layer->tone_curve_points) {
        points.push_back({.x = point.x, .y = point.y});
    }
    return points;
}

[[nodiscard]] QVector<BackendToneCurvePoint> backend_tone_curve_points(
    const ToneCurvePointModel& model
) {
    const auto source = model.points();
    QVector<BackendToneCurvePoint> points;
    points.reserve(source.size());
    for (const auto& point : source) {
        points.push_back({.x = point.x, .y = point.y});
    }
    return points;
}

[[nodiscard]] QString tone_curve_gesture_key(const int index) {
    return QStringLiteral("tone_curve/%1").arg(index);
}

[[nodiscard]] bool layer_list_changed(
    const BackendEditSettings& before,
    const BackendEditSettings& after
) {
    if (before.layers.size() != after.layers.size()) {
        return true;
    }
    for (qsizetype index = 0; index < before.layers.size(); ++index) {
        const auto& left = before.layers.at(index);
        const auto& right = after.layers.at(index);
        if (left.layer_id != right.layer_id || left.label != right.label
            || left.enabled != right.enabled) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] QString history_layer_id(const std::string& key) {
    const QString value = QString::fromStdString(key);
    constexpr auto prefix = "layer/";
    if (!value.startsWith(QLatin1StringView(prefix))) {
        return {};
    }
    const qsizetype end = value.indexOf(QLatin1Char('/'), 6);
    return end < 0 ? value.mid(6) : value.mid(6, end - 6);
}

[[nodiscard]] EditStateTaskResult load_state(
    const std::shared_ptr<DesktopBackend>& backend,
    const QString& photo_id,
    const QString& source_path,
    const quint64 generation
) {
    EditStateTaskResult result;
    result.photo_generation = generation;
    result.kind = EditStateTaskKind::Open;
    try {
        result.state = backend->photoEditState(photo_id, source_path);
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

[[nodiscard]] EditStateTaskResult save_state(
    const std::shared_ptr<DesktopBackend>& backend,
    const QString& photo_id,
    const QString& source_path,
    const QString& base_commit_id,
    const BackendEditSettings settings,
    const QString& version_name,
    const quint64 generation
) {
    EditStateTaskResult result;
    result.photo_generation = generation;
    result.kind = EditStateTaskKind::Save;
    try {
        result.state = backend->saveEditVersion(
            photo_id,
            source_path,
            base_commit_id,
            settings,
            version_name
        );
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

[[nodiscard]] EditStateTaskResult checkout_state(
    const std::shared_ptr<DesktopBackend>& backend,
    const QString& photo_id,
    const QString& source_path,
    const QString& commit_id,
    const quint64 generation
) {
    EditStateTaskResult result;
    result.photo_generation = generation;
    result.kind = EditStateTaskKind::Checkout;
    try {
        result.state = backend->checkoutEditVersion(
            photo_id,
            source_path,
            commit_id
        );
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

[[nodiscard]] EditPreviewTaskResult render_preview(
    const std::shared_ptr<DesktopBackend>& backend,
    const QString& photo_id,
    const QString& source_path,
    const QString& base_commit_id,
    const BackendEditSettings settings,
    const EditPreviewGeneration generation
) {
    EditPreviewTaskResult result;
    result.generation = generation;
    try {
        result.preview = backend->renderEditPreview(
            photo_id,
            source_path,
            base_commit_id,
            settings,
            EDIT_PREVIEW_EDGE,
            EDIT_PREVIEW_QUALITY,
            generation.kind == EditPreviewKind::Current
        );
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

[[nodiscard]] BackendEditedDetailViewport compose_detail_viewport(
    BackendEditedDetailViewport viewport
) {
    if (viewport.full_width == 0 || viewport.full_height == 0 || viewport.tiles.isEmpty()) {
        throw std::runtime_error("full detail returned no RGB8 tiles");
    }
    std::uint32_t left = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t top = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t right = 0;
    std::uint32_t bottom = 0;
    std::uint64_t tile_pixels = 0;
    for (qsizetype index = 0; index < viewport.tiles.size(); ++index) {
        const auto& tile = viewport.tiles.at(index);
        const std::uint64_t expected_stride = static_cast<std::uint64_t>(tile.width) * 3U;
        const std::uint64_t expected_bytes = expected_stride * tile.height;
        const std::uint64_t tile_right = static_cast<std::uint64_t>(tile.x) + tile.width;
        const std::uint64_t tile_bottom = static_cast<std::uint64_t>(tile.y) + tile.height;
        if (tile.width == 0 || tile.height == 0 || tile.row_stride_bytes != expected_stride
            || expected_bytes != static_cast<std::uint64_t>(tile.bytes.size())
            || tile_right > viewport.full_width || tile_bottom > viewport.full_height) {
            throw std::runtime_error("full detail returned an invalid RGB8 tile layout");
        }
        for (qsizetype prior_index = 0; prior_index < index; ++prior_index) {
            const auto& prior = viewport.tiles.at(prior_index);
            const bool overlaps = tile.x < prior.x + prior.width
                && prior.x < tile.x + tile.width && tile.y < prior.y + prior.height
                && prior.y < tile.y + tile.height;
            if (overlaps) {
                throw std::runtime_error("full detail returned overlapping RGB8 tiles");
            }
        }
        left = std::min(left, tile.x);
        top = std::min(top, tile.y);
        right = std::max(right, static_cast<std::uint32_t>(tile_right));
        bottom = std::max(bottom, static_cast<std::uint32_t>(tile_bottom));
        tile_pixels += static_cast<std::uint64_t>(tile.width) * tile.height;
    }

    const std::uint32_t presentation_width = right - left;
    const std::uint32_t presentation_height = bottom - top;
    const std::uint64_t presentation_pixels =
        static_cast<std::uint64_t>(presentation_width) * presentation_height;
    const std::uint64_t presentation_bytes = presentation_pixels * 3U;
    if (presentation_width == 0 || presentation_height == 0
        || tile_pixels != presentation_pixels
        || presentation_bytes
            > static_cast<std::uint64_t>(std::numeric_limits<qsizetype>::max())) {
        throw std::runtime_error("full detail tiles do not cover one complete viewport");
    }
    if (presentation_bytes > EDIT_DETAIL_MAX_PRESENTATION_BYTES) {
        throw std::runtime_error("full detail viewport exceeds the 96 MiB RGB limit");
    }

    QByteArray composite;
    composite.resize(static_cast<qsizetype>(presentation_bytes));
    composite.fill('\0');
    for (const auto& tile : viewport.tiles) {
        for (std::uint32_t row = 0; row < tile.height; ++row) {
            const std::size_t destination_offset =
                (static_cast<std::size_t>(tile.y - top + row) * presentation_width
                 + (tile.x - left))
                * 3U;
            const std::size_t source_offset =
                static_cast<std::size_t>(row) * tile.row_stride_bytes;
            std::memcpy(
                composite.data() + destination_offset,
                tile.bytes.constData() + source_offset,
                static_cast<std::size_t>(tile.row_stride_bytes)
            );
        }
    }
    viewport.tiles = {{
        .bytes = std::move(composite),
        .x = left,
        .y = top,
        .width = presentation_width,
        .height = presentation_height,
        .row_stride_bytes = presentation_width * 3U,
    }};
    return viewport;
}

[[nodiscard]] EditDetailTaskResult render_detail(
    const std::shared_ptr<DesktopBackend>& backend,
    const QString& photo_id,
    const QString& source_path,
    const QString& base_commit_id,
    const BackendEditSettings settings,
    const std::uint64_t render_token,
    const double center_x,
    const double center_y,
    const std::uint32_t viewport_width,
    const std::uint32_t viewport_height,
    const EditDetailGeneration generation
) {
    EditDetailTaskResult result;
    result.generation = generation;
    try {
        const std::uint32_t tile_side = std::max(viewport_width, viewport_height) > 4'096U
            ? EDIT_LARGE_DETAIL_TILE_SIDE
            : EDIT_DETAIL_TILE_SIDE;
        result.viewport = compose_detail_viewport(
            backend->renderEditDetailViewport(
                photo_id,
                source_path,
                base_commit_id,
                settings,
                render_token,
                center_x,
                center_y,
                viewport_width,
                viewport_height,
                tile_side,
                true
            )
        );
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
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
      tone_curve_points_(this) {
    preview_debounce_.setSingleShot(true);
    detail_debounce_.setSingleShot(true);
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
        &detail_watcher_,
        &QFutureWatcher<EditDetailTaskResult>::finished,
        this,
        &EditController::finishDetailTask
    );
}

EditController::~EditController() {
    preview_debounce_.stop();
    detail_debounce_.stop();
    detail_render_token_ = backend_->beginEditDetailRequest();
    state_watcher_.waitForFinished();
    preview_watcher_.waitForFinished();
    detail_watcher_.waitForFinished();
}

bool EditController::active() const noexcept {
    return active_;
}

bool EditController::busy() const noexcept {
    return state_running_ || current_rendering_ || before_rendering_ || detail_rendering_;
}

bool EditController::stateBusy() const noexcept {
    return state_running_;
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
    return detail_error_text_;
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

bool EditController::dirty() const noexcept {
    return dirty_;
}

bool EditController::canUndo() const noexcept {
    return history_.canUndo();
}

bool EditController::canRedo() const noexcept {
    return history_.canRedo();
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

QString EditController::beforePreviewSource() const {
    return before_preview_source_;
}

QString EditController::beforeErrorText() const {
    return before_error_text_;
}

QString EditController::statusText() const {
    return status_text_;
}

QVariantList EditController::layers() const {
    QVariantList result;
    result.reserve(settings_.layers.size());
    for (qsizetype index = 0; index < settings_.layers.size(); ++index) {
        const auto& layer = settings_.layers.at(index);
        QVariantMap item;
        item.insert(QStringLiteral("layerId"), layer.layer_id);
        item.insert(QStringLiteral("label"), layer.label);
        item.insert(QStringLiteral("enabled"), layer.enabled);
        item.insert(QStringLiteral("index"), static_cast<int>(index));
        result.push_back(item);
    }
    return result;
}

int EditController::selectedLayerIndex() const noexcept {
    return selected_layer_index_;
}

QString EditController::selectedLayerId() const {
    const auto* const layer = selectedLayer();
    return layer == nullptr ? QString{} : layer->layer_id;
}

bool EditController::hasSelectedLayer() const noexcept {
    return selectedLayer() != nullptr;
}

bool EditController::canAddLayer() const noexcept {
    return active_ && !state_running_
        && settings_.layers.size() < EditStack::maximum_layer_count;
}

bool EditController::canDeleteLayer() const noexcept {
    return active_ && !state_running_ && hasSelectedLayer()
        && settings_.layers.size() > EditStack::minimum_layer_count;
}

bool EditController::canMoveLayerUp() const noexcept {
    return active_ && !state_running_ && selected_layer_index_ > 0;
}

bool EditController::canMoveLayerDown() const noexcept {
    const int count = static_cast<int>(settings_.layers.size());
    return active_ && !state_running_ && selected_layer_index_ >= 0
        && selected_layer_index_ + 1 < count;
}

bool EditController::layerEnabled() const noexcept {
    const auto* const layer = selectedLayer();
    return layer != nullptr && layer->enabled;
}

double EditController::exposureStops() const noexcept {
    const auto* const layer = selectedLayer();
    return layer == nullptr ? 0.0 : layer->basic.exposure_stops;
}

double EditController::contrastFactor() const noexcept {
    const auto* const layer = selectedLayer();
    return layer == nullptr ? 1.0 : layer->basic.contrast_factor;
}

double EditController::redGain() const noexcept {
    const auto* const layer = selectedLayer();
    return layer == nullptr ? 1.0 : layer->basic.red_channel_gain;
}

double EditController::greenGain() const noexcept {
    const auto* const layer = selectedLayer();
    return layer == nullptr ? 1.0 : layer->basic.green_channel_gain;
}

double EditController::blueGain() const noexcept {
    const auto* const layer = selectedLayer();
    return layer == nullptr ? 1.0 : layer->basic.blue_channel_gain;
}

double EditController::saturationFactor() const noexcept {
    const auto* const layer = selectedLayer();
    return layer == nullptr ? 1.0 : layer->basic.saturation_factor;
}

QAbstractItemModel* EditController::toneCurvePoints() noexcept {
    return &tone_curve_points_;
}

bool EditController::hasToneCurve() const noexcept {
    const auto* const layer = selectedLayer();
    return layer != nullptr && layer->has_tone_curve;
}

bool EditController::toneCurveEditable() const noexcept {
    return tone_curve_points_.isEditable();
}

QAbstractItemModel* EditController::versions() noexcept {
    return &versions_;
}

void EditController::setLayerEnabled(const bool enabled) {
    const auto* const layer = selectedLayer();
    if (!active_ || state_running_ || layer == nullptr || layer->enabled == enabled) {
        return;
    }
    const BackendEditSettings before = settings_;
    const QString layer_id = layer->layer_id;
    settings_.layers[selected_layer_index_].enabled = enabled;
    recordWorkingTransition(
        QStringLiteral("layer/%1/enabled").arg(layer_id),
        before
    );
    emit layersChanged();
    emit layerEnabledChanged();
    setDirty(settings_ != committed_settings_);
    schedulePreview(0);
    setStatusText(
        enabled ? QStringLiteral("Adjustment layer enabled")
                : QStringLiteral("Adjustment layer bypassed · settings preserved")
    );
}

void EditController::setExposureStops(const double value) {
    const auto* const layer = selectedLayer();
    if (layer == nullptr || layer->basic.exposure_stops == value
        || !acceptParameter(value, -16.0, 16.0, QStringLiteral("Exposure"))) {
        return;
    }
    const BackendEditSettings before = settings_;
    settings_.layers[selected_layer_index_].basic.exposure_stops = value;
    parameterEdited(QStringLiteral("exposure"), before);
}

void EditController::setContrastFactor(const double value) {
    const auto* const layer = selectedLayer();
    if (layer == nullptr || layer->basic.contrast_factor == value
        || !acceptParameter(value, 0.0, 8.0, QStringLiteral("Contrast"))) {
        return;
    }
    const BackendEditSettings before = settings_;
    settings_.layers[selected_layer_index_].basic.contrast_factor = value;
    parameterEdited(QStringLiteral("contrast"), before);
}

void EditController::setRedGain(const double value) {
    const auto* const layer = selectedLayer();
    if (layer == nullptr || layer->basic.red_channel_gain == value
        || !acceptParameter(value, 0.000'001, 16.0, QStringLiteral("Red gain"))) {
        return;
    }
    const BackendEditSettings before = settings_;
    settings_.layers[selected_layer_index_].basic.red_channel_gain = value;
    parameterEdited(QStringLiteral("red_gain"), before);
}

void EditController::setGreenGain(const double value) {
    const auto* const layer = selectedLayer();
    if (layer == nullptr || layer->basic.green_channel_gain == value
        || !acceptParameter(value, 0.000'001, 16.0, QStringLiteral("Green gain"))) {
        return;
    }
    const BackendEditSettings before = settings_;
    settings_.layers[selected_layer_index_].basic.green_channel_gain = value;
    parameterEdited(QStringLiteral("green_gain"), before);
}

void EditController::setBlueGain(const double value) {
    const auto* const layer = selectedLayer();
    if (layer == nullptr || layer->basic.blue_channel_gain == value
        || !acceptParameter(value, 0.000'001, 16.0, QStringLiteral("Blue gain"))) {
        return;
    }
    const BackendEditSettings before = settings_;
    settings_.layers[selected_layer_index_].basic.blue_channel_gain = value;
    parameterEdited(QStringLiteral("blue_gain"), before);
}

void EditController::setSaturationFactor(const double value) {
    const auto* const layer = selectedLayer();
    if (layer == nullptr || layer->basic.saturation_factor == value
        || !acceptParameter(value, 0.0, 8.0, QStringLiteral("Saturation"))) {
        return;
    }
    const BackendEditSettings before = settings_;
    settings_.layers[selected_layer_index_].basic.saturation_factor = value;
    parameterEdited(QStringLiteral("saturation"), before);
}

void EditController::openPhoto(
    const QString& photo_id,
    const QString& representation_id,
    const QString& source_path,
    const QString& title
) {
    if (state_running_) {
        setStatusText(QStringLiteral("Finish the current version operation first"));
        return;
    }
    if (dirty_ && active_) {
        setStatusText(QStringLiteral("Save or revert the current changes before reopening a photo"));
        return;
    }
    if (active_ && photo_id == photo_id_ && source_path == source_path_) {
        setStatusText(QStringLiteral("This photo is already open in Precision"));
        return;
    }
    if (photo_id.isEmpty() || representation_id.isEmpty() || source_path.isEmpty()) {
        setStatusText(QStringLiteral("The selected Review item has no editable RAW source"));
        return;
    }

    ++photo_generation_;
    ++render_revision_;
    settled_render_revision_ = 0;
    preview_debounce_.stop();
    resetDetailState();
    preview_queued_ = false;
    before_requested_ = false;
    photo_id_ = photo_id;
    representation_id_ = representation_id;
    source_path_ = source_path;
    title_ = title;
    versions_.replace({});
    working_commit_id_.clear();
    committed_settings_ = {};
    clearSessionHistory();
    setSettings({});
    if (!preview_source_.isEmpty()) {
        preview_source_.clear();
        emit previewSourceChanged();
    }
    if (!before_preview_source_.isEmpty()) {
        before_preview_source_.clear();
        emit beforePreviewSourceChanged();
    }
    if (!before_error_text_.isEmpty()) {
        before_error_text_.clear();
        emit beforeErrorTextChanged();
    }
    preview_store_->clearAll(render_revision_, photo_generation_);
    if (!active_) {
        active_ = true;
        emit activeChanged();
        emit layerActionsChanged();
    }
    emit titleChanged();
    emit sourcePathChanged();
    setStateRunning(true);
    setStatusText(QStringLiteral("Loading non-destructive edit history…"));
    state_watcher_.setFuture(QtConcurrent::run(
        load_state,
        backend_,
        photo_id_,
        source_path_,
        photo_generation_
    ));
}

void EditController::closePhoto() {
    if (dirty_) {
        setStatusText(QStringLiteral("Save or revert the current changes before closing Precision"));
        return;
    }
    if (!active_) {
        return;
    }
    clearSessionHistory();
    resetDetailState();
    active_ = false;
    emit activeChanged();
    emit layerActionsChanged();
    emit historyChanged();
}

void EditController::selectLayer(const int index) {
    const int count = static_cast<int>(settings_.layers.size());
    if (!active_ || state_running_ || index < 0 || index >= count
        || index == selected_layer_index_) {
        return;
    }
    finishActiveGesture();
    setSettings(settings_, settings_.layers.at(index).layer_id);
}

void EditController::addLayer() {
    if (!canAddLayer()) {
        setStatusText(QStringLiteral("An edit can contain at most 16 adjustment layers"));
        return;
    }
    finishActiveGesture();
    BackendBasicEditLayer layer;
    try {
        layer = backend_->newBasicEditLayer(uniqueLayerLabel(QStringLiteral("Basic Adjustments")));
    } catch (const std::exception& error) {
        setStatusText(QStringLiteral("Could not create adjustment layer · %1").arg(
            QString::fromUtf8(error.what())
        ));
        return;
    }
    const BackendEditSettings before = settings_;
    BackendEditSettings updated = settings_;
    int selection = selected_layer_index_;
    if (!EditStack::insertAfterSelection(updated, layer, selection)) {
        setStatusText(QStringLiteral("The adjustment layer could not be inserted safely"));
        return;
    }
    setSettings(std::move(updated), layer.layer_id);
    recordWorkingTransition(
        QStringLiteral("layer/%1/add").arg(layer.layer_id),
        before
    );
    schedulePreview(0);
    setStatusText(QStringLiteral("Added adjustment layer · %1").arg(layer.label));
}

void EditController::duplicateSelectedLayer() {
    const auto* const source = selectedLayer();
    if (!canAddLayer() || source == nullptr) {
        return;
    }
    finishActiveGesture();
    BackendBasicEditLayer duplicate;
    try {
        duplicate = backend_->newBasicEditLayer(
            uniqueLayerLabel(source->label + QStringLiteral(" Copy"))
        );
    } catch (const std::exception& error) {
        setStatusText(QStringLiteral("Could not duplicate adjustment layer · %1").arg(
            QString::fromUtf8(error.what())
        ));
        return;
    }
    duplicate.basic = source->basic;
    duplicate.enabled = source->enabled;
    duplicate.has_tone_curve = source->has_tone_curve;
    duplicate.tone_curve_points = source->tone_curve_points;

    const BackendEditSettings before = settings_;
    BackendEditSettings updated = settings_;
    int selection = selected_layer_index_;
    if (!EditStack::insertAfterSelection(updated, duplicate, selection)) {
        setStatusText(QStringLiteral("The duplicate layer could not be inserted safely"));
        return;
    }
    setSettings(std::move(updated), duplicate.layer_id);
    recordWorkingTransition(
        QStringLiteral("layer/%1/duplicate").arg(duplicate.layer_id),
        before
    );
    schedulePreview(0);
    setStatusText(QStringLiteral("Duplicated adjustment layer · %1").arg(duplicate.label));
}

void EditController::deleteSelectedLayer() {
    const auto* const selected = selectedLayer();
    if (!canDeleteLayer() || selected == nullptr) {
        return;
    }
    finishActiveGesture();
    const QString deleted_id = selected->layer_id;
    const QString deleted_label = selected->label;
    const BackendEditSettings before = settings_;
    BackendEditSettings updated = settings_;
    int selection = selected_layer_index_;
    if (!EditStack::deleteSelection(updated, selection)) {
        return;
    }
    const QString next_id = updated.layers.at(selection).layer_id;
    setSettings(std::move(updated), next_id);
    recordWorkingTransition(
        QStringLiteral("layer/%1/delete").arg(deleted_id),
        before
    );
    schedulePreview(0);
    setStatusText(QStringLiteral("Deleted adjustment layer · %1").arg(deleted_label));
}

void EditController::moveSelectedLayer(const int destination_index) {
    const auto* const selected = selectedLayer();
    if (!active_ || state_running_ || selected == nullptr) {
        return;
    }
    finishActiveGesture();
    const QString moved_id = selected->layer_id;
    const BackendEditSettings before = settings_;
    BackendEditSettings updated = settings_;
    int selection = selected_layer_index_;
    if (!EditStack::moveSelection(updated, selection, destination_index)) {
        return;
    }
    setSettings(std::move(updated), moved_id);
    recordWorkingTransition(
        QStringLiteral("layer/%1/move").arg(moved_id),
        before
    );
    schedulePreview(0);
    setStatusText(QStringLiteral("Reordered adjustment layer"));
}

void EditController::beginParameterEdit(const QString& parameter_key) {
    const auto* const layer = selectedLayer();
    if (!active_ || state_running_ || layer == nullptr || !layer->enabled
        || parameter_key.isEmpty()) {
        return;
    }
    const bool could_undo = canUndo();
    const bool could_redo = canRedo();
    history_.beginGesture(layerHistoryKey(parameter_key).toStdString(), settings_);
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
    history_.endGesture(layerHistoryKey(parameter_key).toStdString(), settings_);
    if (could_undo != canUndo() || could_redo != canRedo()) {
        emit historyChanged();
    }
}

void EditController::beginToneCurveGesture(const int index) {
    const auto* const layer = selectedLayer();
    if (!active_ || state_running_ || layer == nullptr || !layer->enabled
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
    const auto* const layer = selectedLayer();
    if (!active_ || state_running_ || layer == nullptr || !layer->enabled) {
        return;
    }
    const BackendEditSettings before = settings_;
    if (!tone_curve_points_.movePoint(index, x, y)) {
        return;
    }
    auto& edited = settings_.layers[selected_layer_index_];
    edited.has_tone_curve = true;
    edited.tone_curve_points = backend_tone_curve_points(tone_curve_points_);
    toneCurveEdited(tone_curve_gesture_key(index), before, EDIT_DEBOUNCE_MS);
}

void EditController::endToneCurveGesture(const int index) {
    endParameterEdit(tone_curve_gesture_key(index));
}

void EditController::addToneCurvePoint(const double x, const double y) {
    const auto* const layer = selectedLayer();
    if (!active_ || state_running_ || layer == nullptr || !layer->enabled) {
        return;
    }
    const BackendEditSettings before = settings_;
    if (tone_curve_points_.addPoint(x, y) < 0) {
        setStatusText(QStringLiteral("The point cannot be added inside this curve"));
        return;
    }
    auto& edited = settings_.layers[selected_layer_index_];
    edited.has_tone_curve = true;
    edited.tone_curve_points = backend_tone_curve_points(tone_curve_points_);
    toneCurveEdited(QStringLiteral("tone_curve/add"), before, 0);
}

void EditController::removeToneCurvePoint(const int index) {
    const auto* const layer = selectedLayer();
    if (!active_ || state_running_ || layer == nullptr || !layer->enabled) {
        return;
    }
    const BackendEditSettings before = settings_;
    if (!tone_curve_points_.removePoint(index)) {
        return;
    }
    settings_.layers[selected_layer_index_].tone_curve_points =
        backend_tone_curve_points(tone_curve_points_);
    toneCurveEdited(QStringLiteral("tone_curve/remove"), before, 0);
}

void EditController::resetToneCurve() {
    const auto* const layer = selectedLayer();
    if (!active_ || state_running_ || layer == nullptr || !layer->enabled
        || !layer->has_tone_curve) {
        return;
    }
    const BackendEditSettings before = settings_;
    tone_curve_points_.resetLinear();
    auto& edited = settings_.layers[selected_layer_index_];
    edited.has_tone_curve = false;
    edited.tone_curve_points.clear();
    toneCurveEdited(QStringLiteral("tone_curve/reset"), before, 0);
}

void EditController::undo() {
    if (!active_ || state_running_) {
        return;
    }
    std::string history_key;
    const auto restored = history_.undo(settings_, &history_key);
    emit historyChanged();
    if (!restored) {
        return;
    }
    QString preferred_id = history_layer_id(history_key);
    const bool undoes_insert = history_key.ends_with("/add")
        || history_key.ends_with("/duplicate");
    if (undoes_insert && EditStack::layerIndex(*restored, preferred_id) < 0
        && !restored->layers.isEmpty()) {
        const int previous_index = std::clamp(
            selected_layer_index_ - 1,
            0,
            static_cast<int>(restored->layers.size() - 1)
        );
        preferred_id = restored->layers.at(previous_index).layer_id;
    }
    setSettings(*restored, preferred_id);
    schedulePreview(0);
    setStatusText(QStringLiteral("Undid the last session adjustment"));
}

void EditController::redo() {
    if (!active_ || state_running_) {
        return;
    }
    std::string history_key;
    const auto restored = history_.redo(settings_, &history_key);
    emit historyChanged();
    if (!restored) {
        return;
    }
    setSettings(*restored, history_layer_id(history_key));
    schedulePreview(0);
    setStatusText(QStringLiteral("Redid the last session adjustment"));
}

void EditController::resetEdits() {
    const auto* const layer = selectedLayer();
    if (!active_ || state_running_ || layer == nullptr || !layer->enabled) {
        return;
    }
    const BackendBasicEditParameters neutral;
    if (layer->basic == neutral) {
        return;
    }
    const BackendEditSettings before = settings_;
    BackendEditSettings reset = settings_;
    reset.layers[selected_layer_index_].basic = neutral;
    const QString layer_id = layer->layer_id;
    setSettings(std::move(reset), layer_id);
    recordWorkingTransition(
        QStringLiteral("layer/%1/reset").arg(layer_id),
        before
    );
    schedulePreview(0);
}

void EditController::revertEdits() {
    if (!active_ || state_running_) {
        return;
    }
    if (settings_ != committed_settings_) {
        const BackendEditSettings before = settings_;
        setSettings(committed_settings_);
        recordWorkingTransition(QStringLiteral("revert"), before);
        schedulePreview(0);
    }
    setStatusText(QStringLiteral("Restored the current saved version"));
}

void EditController::requestBeforePreview() {
    if (!active_ || !before_preview_source_.isEmpty()) {
        return;
    }
    if (!before_error_text_.isEmpty()) {
        before_error_text_.clear();
        emit beforeErrorTextChanged();
    }
    before_requested_ = true;
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
    if (!detail_error_text_.isEmpty()) {
        detail_error_text_.clear();
        emit detailErrorTextChanged();
    }
    invalidateDetailPresentation();
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
    if (!detail_error_text_.isEmpty()) {
        detail_error_text_.clear();
        emit detailErrorTextChanged();
    }
}

void EditController::saveVersion(const QString& version_name) {
    const QString name = version_name.trimmed();
    if (!active_ || state_running_) {
        return;
    }
    if (name.isEmpty()) {
        setStatusText(QStringLiteral("Enter a name for this version"));
        return;
    }
    history_.finishGesture(settings_);
    emit historyChanged();
    setStateRunning(true);
    setStatusText(QStringLiteral("Saving immutable version “%1”…").arg(name));
    state_watcher_.setFuture(QtConcurrent::run(
        save_state,
        backend_,
        photo_id_,
        source_path_,
        working_commit_id_,
        settings_,
        name,
        photo_generation_
    ));
}

void EditController::checkoutVersion(const QString& commit_id) {
    if (!active_ || state_running_ || commit_id.isEmpty()) {
        return;
    }
    if (dirty_) {
        setStatusText(QStringLiteral("Save or revert current changes before checking out another version"));
        return;
    }
    setStateRunning(true);
    setStatusText(QStringLiteral("Checking out an immutable version…"));
    state_watcher_.setFuture(QtConcurrent::run(
        checkout_state,
        backend_,
        photo_id_,
        source_path_,
        commit_id,
        photo_generation_
    ));
}

void EditController::finishStateTask() {
    EditStateTaskResult result = state_watcher_.result();
    setStateRunning(false);
    if (result.photo_generation != photo_generation_) {
        return;
    }
    if (!result.error.isEmpty()) {
        if (result.kind == EditStateTaskKind::Open && active_) {
            active_ = false;
            emit activeChanged();
            emit layerActionsChanged();
        }
        setStatusText(QStringLiteral("Version operation failed · %1").arg(result.error));
        if (preview_queued_) {
            preview_debounce_.start(0);
        }
        maybeStartBeforePreview();
        maybeStartDetailRender();
        return;
    }
    applyState(std::move(result.state));
    switch (result.kind) {
    case EditStateTaskKind::Open:
        setStatusText(QStringLiteral("Edit history ready · rendering scene-linear preview"));
        schedulePreview(0);
        break;
    case EditStateTaskKind::Save:
        setStatusText(QStringLiteral("Version saved · the previous head remains available"));
        break;
    case EditStateTaskKind::Checkout:
        setStatusText(QStringLiteral("Version checked out · new edits will branch from here"));
        schedulePreview(0);
        break;
    }
    if (preview_queued_ && !preview_debounce_.isActive()) {
        preview_debounce_.start(0);
    }
    maybeStartBeforePreview();
    maybeStartDetailRender();
}

void EditController::finishPreviewTask() {
    EditPreviewTaskResult result = preview_watcher_.result();
    setPreviewRunning(result.generation.kind, false);
    const bool accepted = active_ && accepts_edit_preview(
        result.generation,
        photo_generation_,
        render_revision_
    );

    if (result.generation.kind == EditPreviewKind::Current && accepted) {
        settled_render_revision_ = result.generation.current_revision;
        if (!result.error.isEmpty()) {
            setStatusText(QStringLiteral("Preview render failed · %1").arg(result.error));
        } else {
            const QSize dimensions(
                static_cast<int>(result.preview.width),
                static_cast<int>(result.preview.height)
            );
            preview_store_->publish(
                EditPreviewSlot::Current,
                std::move(result.preview.bytes),
                dimensions,
                result.generation.current_revision
            );
            preview_source_ = QStringLiteral("image://shadow-edit/current?generation=%1")
                                  .arg(result.generation.current_revision);
            emit previewSourceChanged();
            setStatusText(
                dirty_ ? QStringLiteral("Unsaved changes · preview is current")
                       : QStringLiteral("Version and preview are current")
            );
        }
    } else if (result.generation.kind == EditPreviewKind::NeutralBefore && accepted) {
        before_requested_ = false;
        if (!result.error.isEmpty()) {
            before_error_text_ = QStringLiteral("Neutral baseline failed · %1").arg(
                result.error
            );
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
        schedulePreview(0);
    } else {
        maybeStartBeforePreview();
        maybeStartDetailRender();
    }
}

void EditController::finishDetailTask() {
    EditDetailTaskResult result = detail_watcher_.result();
    setDetailRunning(false);
    const bool accepted = detail_mode_ && active_ && accepts_edit_detail(
        result.generation,
        photo_generation_,
        render_revision_,
        detail_viewport_revision_
    );
    if (accepted) {
        if (!result.error.isEmpty()) {
            detail_error_text_ = QStringLiteral("Full detail failed · %1").arg(
                result.error
            );
            emit detailErrorTextChanged();
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
                detail_error_text_ = QStringLiteral(
                    "Full detail returned an invalid RGB8 tile layout"
                );
                emit detailErrorTextChanged();
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
                emit detailTilesChanged();
                const double retained_mib = static_cast<double>(detail_retained_bytes_)
                    / (1'024.0 * 1'024.0);
                setStatusText(
                    QStringLiteral("Full-resolution detail ready · %1 MiB local source")
                        .arg(retained_mib, 0, 'f', 0)
                );
            }
        }
    }

    if (detail_queued_) {
        maybeStartDetailRender();
    } else {
        maybeStartBeforePreview();
    }
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
    setStatusText(QStringLiteral("Rendering bounded scene-linear preview…"));
    preview_watcher_.setFuture(QtConcurrent::run(
        render_preview,
        backend_,
        photo_id_,
        source_path_,
        working_commit_id_,
        settings_,
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
    setStatusText(QStringLiteral("Preparing exact full-resolution detail…"));
    detail_watcher_.setFuture(QtConcurrent::run(
        render_detail,
        backend_,
        photo_id_,
        source_path_,
        working_commit_id_,
        settings_,
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
        render_preview,
        backend_,
        photo_id_,
        source_path_,
        QString{},
        BackendEditSettings{},
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

void EditController::invalidateDetailPresentation() {
    detail_render_token_ = backend_->beginEditDetailRequest();
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

void EditController::resetDetailState() {
    detail_debounce_.stop();
    detail_queued_ = false;
    ++detail_viewport_revision_;
    invalidateDetailPresentation();
    if (detail_mode_) {
        detail_mode_ = false;
        emit detailModeChanged();
    }
    if (!detail_error_text_.isEmpty()) {
        detail_error_text_.clear();
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
}

void EditController::applyState(BackendPhotoEditState state) {
    if (state.photo_id != photo_id_ || state.source_path != source_path_) {
        setStatusText(QStringLiteral("Catalog returned edit state for a different photo"));
        return;
    }
    committed_settings_ = state.settings;
    working_commit_id_ = std::move(state.working_commit_id);
    setSettings(std::move(state.settings));
    clearSessionHistory();
    versions_.replace(std::move(state.versions));
}

void EditController::setSettings(
    BackendEditSettings settings,
    const QString& preferred_layer_id
) {
    if (settings.layers.size() > EditStack::maximum_layer_count) {
        setStatusText(QStringLiteral("The saved edit exceeds the 16-layer desktop limit"));
        return;
    }
    for (auto& layer : settings.layers) {
        if (!layer.has_tone_curve) {
            layer.tone_curve_points.clear();
        }
    }

    const QString old_selected_id = selectedLayerId();
    const int old_selected_index = selected_layer_index_;
    const BackendBasicEditLayer* const old_selected = selectedLayer();
    const bool had_old_selection = old_selected != nullptr;
    const BackendBasicEditLayer old_selected_value = had_old_selection
        ? *old_selected
        : BackendBasicEditLayer{};
    const QString requested_id = preferred_layer_id.isEmpty()
        ? old_selected_id
        : preferred_layer_id;
    const int new_selected_index = EditStack::resolvedSelection(
        settings,
        requested_id,
        old_selected_index
    );
    const BackendBasicEditLayer* const new_selected = new_selected_index < 0
        ? nullptr
        : &settings.layers.at(new_selected_index);
    const bool has_new_selection = new_selected != nullptr;
    const bool selection_changed = old_selected_index != new_selected_index
        || old_selected_id
            != (has_new_selection ? new_selected->layer_id : QString{});
    const bool layer_enabled_changed = selection_changed
        || had_old_selection != has_new_selection
        || (had_old_selection && has_new_selection
            && old_selected_value.enabled != new_selected->enabled);
    const bool basic_changed = selection_changed
        || had_old_selection != has_new_selection
        || (had_old_selection && has_new_selection
            && old_selected_value.basic != new_selected->basic);
    const bool curve_changed = selection_changed
        || had_old_selection != has_new_selection
        || (had_old_selection && has_new_selection
            && (old_selected_value.has_tone_curve != new_selected->has_tone_curve
                || old_selected_value.tone_curve_points
                    != new_selected->tone_curve_points));
    const bool list_changed = layer_list_changed(settings_, settings);
    const auto model_points = tone_curve_model_points(new_selected);
    if (tone_curve_points_.points() != model_points
        && !tone_curve_points_.replace(model_points)) {
        setStatusText(QStringLiteral("The saved Tone Curve cannot be represented safely"));
        return;
    }
    settings_ = std::move(settings);
    selected_layer_index_ = new_selected_index;
    if (list_changed) {
        emit layersChanged();
    }
    if (selection_changed) {
        emit selectedLayerChanged();
    }
    if (list_changed || selection_changed) {
        emit layerActionsChanged();
    }
    if (layer_enabled_changed) {
        emit layerEnabledChanged();
    }
    if (basic_changed) {
        emit parametersChanged();
    }
    if (curve_changed) {
        emit toneCurveChanged();
    }
    setDirty(settings_ != committed_settings_);
}

const BackendBasicEditLayer* EditController::selectedLayer() const noexcept {
    const int count = static_cast<int>(settings_.layers.size());
    if (selected_layer_index_ < 0 || selected_layer_index_ >= count) {
        return nullptr;
    }
    return &settings_.layers.at(selected_layer_index_);
}

QString EditController::layerHistoryKey(const QString& key) const {
    const auto* const layer = selectedLayer();
    return layer == nullptr
        ? key
        : QStringLiteral("layer/%1/%2").arg(layer->layer_id, key);
}

QString EditController::uniqueLayerLabel(const QString& base) const {
    const QString clean_base = base.trimmed().isEmpty()
        ? QStringLiteral("Basic Adjustments")
        : base.trimmed();
    const auto exists = [this](const QString& candidate) {
        return std::any_of(
            settings_.layers.cbegin(),
            settings_.layers.cend(),
            [&candidate](const BackendBasicEditLayer& layer) {
                return layer.label == candidate;
            }
        );
    };
    if (!exists(clean_base)) {
        return clean_base;
    }
    for (int suffix = 2; suffix <= EditStack::maximum_layer_count + 1; ++suffix) {
        const QString candidate = QStringLiteral("%1 %2").arg(clean_base).arg(suffix);
        if (!exists(candidate)) {
            return candidate;
        }
    }
    return clean_base + QStringLiteral(" Copy");
}

void EditController::finishActiveGesture() {
    const bool could_undo = canUndo();
    const bool could_redo = canRedo();
    history_.finishGesture(settings_);
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
    const BackendEditSettings& before
) {
    const bool could_undo = canUndo();
    const bool could_redo = canRedo();
    history_.record(key.toStdString(), before, settings_);
    if (could_undo != canUndo() || could_redo != canRedo()) {
        emit historyChanged();
    }
}

void EditController::schedulePreview(const int delay_ms) {
    if (!active_) {
        return;
    }
    ++render_revision_;
    if (detail_mode_) {
        invalidateDetailPresentation();
        detail_queued_ = true;
        detail_debounce_.start(std::max(delay_ms, EDIT_DETAIL_DEBOUNCE_MS));
    }
    if (current_rendering_ || before_rendering_) {
        preview_queued_ = true;
    }
    preview_debounce_.start(delay_ms);
}

void EditController::setStatusText(QString status) {
    if (status_text_ == status) {
        return;
    }
    status_text_ = std::move(status);
    emit statusTextChanged();
}

void EditController::setDirty(const bool dirty) {
    if (dirty_ == dirty) {
        return;
    }
    dirty_ = dirty;
    emit dirtyChanged();
}

void EditController::setStateRunning(const bool running) {
    if (state_running_ == running) {
        return;
    }
    const bool previous_busy = busy();
    state_running_ = running;
    emit stateBusyChanged();
    emit layerActionsChanged();
    emitBusyChange(previous_busy);
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

void EditController::parameterEdited(
    const QString& key,
    const BackendEditSettings& before
) {
    if (!active_ || state_running_) {
        return;
    }
    recordWorkingTransition(layerHistoryKey(key), before);
    emit parametersChanged();
    setDirty(settings_ != committed_settings_);
    schedulePreview(EDIT_DEBOUNCE_MS);
}

void EditController::toneCurveEdited(
    const QString& key,
    const BackendEditSettings& before,
    const int preview_delay_ms
) {
    recordWorkingTransition(layerHistoryKey(key), before);
    emit toneCurveChanged();
    setDirty(settings_ != committed_settings_);
    schedulePreview(preview_delay_ms);
}

bool EditController::acceptParameter(
    const double value,
    const double minimum,
    const double maximum,
    const QString& label
) {
    if (!active_ || state_running_) {
        return false;
    }
    const auto* const layer = selectedLayer();
    if (layer == nullptr) {
        setStatusText(QStringLiteral("Select an adjustment layer before editing"));
        return false;
    }
    if (!layer->enabled) {
        setStatusText(QStringLiteral("Enable the selected layer before editing its controls"));
        return false;
    }
    if (!std::isfinite(value) || value < minimum || value > maximum) {
        setStatusText(
            QStringLiteral("%1 is outside the supported preview range").arg(label)
        );
        return false;
    }
    return true;
}
