#include "edit_preview_texture_item.hpp"

#include "edit_preview_liquify_mesh.hpp"
#include "edit_preview_metal_texture_factory.hpp"
#include "edit_preview_presentation_context.hpp"
#include "edit_preview_presentation_registry.hpp"
#include "edit_preview_provider.hpp"

#include <QMetaObject>
#include <QPointer>
#include <QQuickTextureFactory>
#include <QQuickWindow>
#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGTexture>
#include <QSGTextureMaterial>
#include <QSizeF>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace {

struct ParsedPreviewSource final {
    EditPreviewSlot slot = EditPreviewSlot::Current;
    quint64 generation = 0U;
};

struct EditPreviewTextureIdentity final {
    QString source;
    QString generation;
    const BackendEditPreviewFrame* frame_owner = nullptr;
    EditPreviewPresentationBinding presentation_binding;
    std::uint64_t configuration_revision = 0U;
    std::uint64_t source_binding_revision = 0U;
    std::uint64_t scene_graph_revision = 0U;

    bool operator==(const EditPreviewTextureIdentity&) const = default;
};

struct TransientLiquifyState final {
    std::vector<EditPreviewLiquifySample> points;
    QString base_source;
    double radius = 0.0;
    double strength = 0.0;
    double hardness = 0.0;
    std::uint64_t identity = 0U;
    std::uint64_t revision = 0U;
    bool active = false;
    bool awaiting_authoritative_frame = false;
};

[[nodiscard]] std::optional<ParsedPreviewSource> parse_preview_source(const QString& source) {
    const QUrl url(source);
    if (url.scheme() != QStringLiteral("image") || url.host() != QStringLiteral("shadow-edit")) {
        return std::nullopt;
    }
    EditPreviewSlot slot;
    if (url.path() == QStringLiteral("/current")) {
        slot = EditPreviewSlot::Current;
    } else if (url.path() == QStringLiteral("/before")) {
        slot = EditPreviewSlot::Before;
    } else {
        return std::nullopt;
    }
    bool valid_generation = false;
    const quint64 generation =
        QUrlQuery(url).queryItemValue(QStringLiteral("generation")).toULongLong(&valid_generation);
    if (!valid_generation) {
        return std::nullopt;
    }
    return ParsedPreviewSource{
        .slot = slot,
        .generation = generation,
    };
}

[[nodiscard]] constexpr std::uint64_t next_revision(const std::uint64_t current) noexcept {
    return current == std::numeric_limits<std::uint64_t>::max() ? 1U : current + 1U;
}

[[nodiscard]] std::uint64_t advance_revision(std::atomic<std::uint64_t>& revision) noexcept {
    std::uint64_t current = revision.load(std::memory_order_acquire);
    for (;;) {
        const std::uint64_t next = next_revision(current);
        if (revision.compare_exchange_weak(
                current,
                next,
                std::memory_order_acq_rel,
                std::memory_order_acquire
            )) {
            return next;
        }
    }
}

class EditPreviewTextureNode final : public QSGGeometryNode {
  public:
    EditPreviewTextureNode() :
        geometry_(new QSGGeometry(
            QSGGeometry::defaultAttributes_TexturedPoint2D(),
            0,
            0,
            QSGGeometry::UnsignedShortType
        )),
        material_(new QSGTextureMaterial()) {
        geometry_->setDrawingMode(QSGGeometry::DrawTriangles);
        setGeometry(geometry_);
        setFlag(QSGNode::OwnsGeometry);
        material_->setFiltering(QSGTexture::Linear);
        setMaterial(material_);
        setFlag(QSGNode::OwnsMaterial);
    }

    ~EditPreviewTextureNode() override {
        material_->setTexture(nullptr);
        delete texture_;
    }

    void replaceTexture(QSGTexture* const texture) {
        if (texture_ == texture) {
            return;
        }
        material_->setTexture(nullptr);
        delete texture_;
        texture_ = texture;
        material_->setTexture(texture_);
        markDirty(QSGNode::DirtyMaterial);
    }

    void syncGeometry(const QRectF target_rect, const TransientLiquifyState& transient) {
        if (texture_ == nullptr) {
            return;
        }
        const QRectF texture_rect = texture_->normalizedTextureSubRect();
        if (!transient.active) {
            if (geometry_mode_ == GeometryMode::Quad && target_rect_ == target_rect
                && texture_rect_ == texture_rect) {
                return;
            }
            syncQuad(target_rect, texture_rect);
            geometry_mode_ = GeometryMode::Quad;
            target_rect_ = target_rect;
            texture_rect_ = texture_rect;
            transient_identity_ = transient.identity;
            transient_revision_ = transient.revision;
            return;
        }

        const bool requires_reset = geometry_mode_ != GeometryMode::Liquify
                                    || transient_identity_ != transient.identity
                                    || target_rect_ != target_rect || texture_rect_ != texture_rect
                                    || liquify_mesh_.pointCount() > transient.points.size();
        if (requires_reset) {
            liquify_mesh_.reset(target_rect, texture_rect);
        }
        const std::size_t first_point = requires_reset ? 0U : liquify_mesh_.pointCount();
        for (std::size_t index = first_point; index < transient.points.size(); ++index) {
            static_cast<void>(liquify_mesh_.appendNormalizedPoint(
                transient.points[index].point,
                transient.points[index].pressure,
                transient.radius,
                transient.strength,
                transient.hardness
            ));
        }
        if (!requires_reset && transient_revision_ == transient.revision
            && first_point == transient.points.size()) {
            return;
        }
        syncLiquifyMesh();
        geometry_mode_ = GeometryMode::Liquify;
        target_rect_ = target_rect;
        texture_rect_ = texture_rect;
        transient_identity_ = transient.identity;
        transient_revision_ = transient.revision;
    }

    EditPreviewTextureIdentity texture_identity;

  private:
    enum class GeometryMode {
        None,
        Quad,
        Liquify,
    };

    void syncQuad(const QRectF target_rect, const QRectF texture_rect) {
        geometry_->allocate(4, 6);
        auto* const vertices = geometry_->vertexDataAsTexturedPoint2D();
        vertices[0].set(
            static_cast<float>(target_rect.left()),
            static_cast<float>(target_rect.top()),
            static_cast<float>(texture_rect.left()),
            static_cast<float>(texture_rect.top())
        );
        vertices[1].set(
            static_cast<float>(target_rect.right()),
            static_cast<float>(target_rect.top()),
            static_cast<float>(texture_rect.right()),
            static_cast<float>(texture_rect.top())
        );
        vertices[2].set(
            static_cast<float>(target_rect.left()),
            static_cast<float>(target_rect.bottom()),
            static_cast<float>(texture_rect.left()),
            static_cast<float>(texture_rect.bottom())
        );
        vertices[3].set(
            static_cast<float>(target_rect.right()),
            static_cast<float>(target_rect.bottom()),
            static_cast<float>(texture_rect.right()),
            static_cast<float>(texture_rect.bottom())
        );
        auto* const indices = geometry_->indexDataAsUShort();
        std::copy_n(std::array<std::uint16_t, 6U>{0U, 2U, 1U, 1U, 2U, 3U}.cbegin(), 6U, indices);
        markDirty(QSGNode::DirtyGeometry);
    }

    void syncLiquifyMesh() {
        const auto mesh_vertices = liquify_mesh_.vertices();
        const auto mesh_indices = liquify_mesh_.indices();
        geometry_->allocate(
            static_cast<int>(mesh_vertices.size()),
            static_cast<int>(mesh_indices.size())
        );
        auto* const vertices = geometry_->vertexDataAsTexturedPoint2D();
        for (std::size_t index = 0U; index < mesh_vertices.size(); ++index) {
            const auto& source = mesh_vertices[index];
            vertices[index].set(source.x, source.y, source.texture_x, source.texture_y);
        }
        std::copy(mesh_indices.begin(), mesh_indices.end(), geometry_->indexDataAsUShort());
        markDirty(QSGNode::DirtyGeometry);
    }

    QSGGeometry* geometry_ = nullptr;
    QSGTextureMaterial* material_ = nullptr;
    QSGTexture* texture_ = nullptr;
    EditPreviewLiquifyMesh liquify_mesh_;
    QRectF target_rect_;
    QRectF texture_rect_;
    std::uint64_t transient_identity_ = 0U;
    std::uint64_t transient_revision_ = 0U;
    GeometryMode geometry_mode_ = GeometryMode::None;
};

} // namespace

struct EditPreviewTextureItem::State final {
    struct LiveFrame final {
        std::shared_ptr<const BackendEditPreviewFrame> frame;
        EditPreviewPresentationBinding presentation_binding;
        std::shared_ptr<EditPreviewPresentationContext> presentation_context;
        QSize dimensions;
        QString source_identity;
        QString generation;
        std::uint64_t configuration_revision = 0U;
        std::uint64_t source_binding_revision = 0U;

        [[nodiscard]] EditPreviewTextureIdentity
        textureIdentity(const std::uint64_t scene_graph_revision) const {
            return {
                .source = source_identity,
                .generation = generation,
                .frame_owner = frame.get(),
                .presentation_binding = presentation_binding,
                .configuration_revision = configuration_revision,
                .source_binding_revision = source_binding_revision,
                .scene_graph_revision = scene_graph_revision,
            };
        }
    };

    QPointer<EditPreviewPresentationRegistry> presentation_registry;
    QMetaObject::Connection registry_configuration_connection;
    QMetaObject::Connection registry_destroyed_connection;
    QMetaObject::Connection window_invalidated_connection;
    QMetaObject::Connection window_initialized_connection;
    QPointer<QQuickWindow> observed_window;
    QString source;
    QString fallback_source;
    QString presented_generation;
    std::optional<LiveFrame> live_frame;
    bool live_admission_enabled = true;
    std::uint64_t source_binding_revision = 0U;
    std::atomic<std::uint64_t> scene_graph_revision{1U};
    std::uint64_t window_observation_revision = 0U;
    TransientLiquifyState transient_liquify;
    FillMode fill_mode = FillMode::Stretch;
};

EditPreviewTextureItem::EditPreviewTextureItem(QQuickItem* const parent) :
    QQuickItem(parent), state_(std::make_unique<State>()) {
    setFlag(ItemHasContents, true);
    QObject::connect(
        this,
        &QQuickItem::windowChanged,
        this,
        [this](QQuickWindow* const next_window) { bindWindowLifecycle(next_window); }
    );
    bindWindowLifecycle(window());
}

EditPreviewTextureItem::~EditPreviewTextureItem() = default;

QString EditPreviewTextureItem::source() const {
    return state_->source;
}

QObject* EditPreviewTextureItem::presentationRegistry() const noexcept {
    return state_->presentation_registry;
}

void EditPreviewTextureItem::setPresentationRegistry(QObject* const registry_object) {
    auto* const registry = qobject_cast<EditPreviewPresentationRegistry*>(registry_object);
    if (state_->presentation_registry == registry) {
        return;
    }
    QObject::disconnect(state_->registry_configuration_connection);
    QObject::disconnect(state_->registry_destroyed_connection);
    state_->presentation_registry = registry;
    if (registry != nullptr) {
        state_->registry_configuration_connection = QObject::connect(
            registry,
            &EditPreviewPresentationRegistry::configurationChanged,
            this,
            [this] { refreshSourceBinding(); }
        );
        state_->registry_destroyed_connection =
            QObject::connect(registry, &QObject::destroyed, this, [this] {
                state_->presentation_registry = nullptr;
                emit presentationRegistryChanged();
                refreshSourceBinding();
            });
    }
    emit presentationRegistryChanged();
    refreshSourceBinding();
}

void EditPreviewTextureItem::setSource(const QString& source_value) {
    if (state_->source == source_value) {
        return;
    }
    state_->source = source_value;
    if (state_->transient_liquify.awaiting_authoritative_frame
        && state_->transient_liquify.base_source != source_value) {
        clearTransientLiquify();
    }
    emit sourceChanged();
    refreshSourceBinding();
}

bool EditPreviewTextureItem::liveAdmissionEnabled() const noexcept {
    return state_->live_admission_enabled;
}

void EditPreviewTextureItem::setLiveAdmissionEnabled(const bool enabled) {
    if (state_->live_admission_enabled == enabled) {
        return;
    }
    state_->live_admission_enabled = enabled;
    emit liveAdmissionEnabledChanged();
    refreshSourceBinding();
}

QString EditPreviewTextureItem::fallbackSource() const {
    return state_->fallback_source;
}

bool EditPreviewTextureItem::liveFrameAvailable() const noexcept {
    return state_->live_frame.has_value();
}

QString EditPreviewTextureItem::presentedGeneration() const {
    return state_->presented_generation;
}

bool EditPreviewTextureItem::transientLiquifyActive() const noexcept {
    return state_->transient_liquify.active;
}

bool EditPreviewTextureItem::beginTransientLiquify(
    const double radius,
    const double strength,
    const double hardness
) {
    if (!state_->live_frame.has_value() || !std::isfinite(radius) || radius <= 0.0 || radius > 1.0
        || !std::isfinite(strength) || strength <= 0.0 || strength > 1.0 || !std::isfinite(hardness)
        || hardness < 0.0 || hardness > 1.0) {
        return false;
    }
    auto& transient = state_->transient_liquify;
    const bool was_active = transient.active;
    transient.points.clear();
    transient.base_source = state_->source;
    transient.radius = radius;
    transient.strength = strength;
    transient.hardness = hardness;
    transient.identity = next_revision(transient.identity);
    transient.revision = next_revision(transient.revision);
    transient.active = true;
    transient.awaiting_authoritative_frame = false;
    if (!was_active) {
        emit transientLiquifyChanged();
    }
    update();
    return true;
}

bool EditPreviewTextureItem::appendTransientLiquifyPoint(
    const double x,
    const double y,
    const double pressure
) {
    auto& transient = state_->transient_liquify;
    constexpr std::size_t maximum_points = 2'048U;
    if (!transient.active || !std::isfinite(x) || !std::isfinite(y) || x < 0.0 || x > 1.0 || y < 0.0
        || y > 1.0 || !std::isfinite(pressure) || pressure < 0.0 || pressure > 1.0) {
        return false;
    }
    if (transient.points.size() >= maximum_points) {
        std::vector<EditPreviewLiquifySample> compacted;
        compacted.reserve((transient.points.size() + 1U) / 2U);
        for (std::size_t index = 0U; index < transient.points.size(); index += 2U) {
            compacted.push_back(transient.points[index]);
        }
        transient.points = std::move(compacted);
        // The render-thread mesh must replay the compacted prefix rather than
        // continue from vertex state produced by the discarded samples.
        transient.identity = next_revision(transient.identity);
    }
    transient.points.push_back(
        EditPreviewLiquifySample{
            .point = QPointF{x, y},
            .pressure = pressure,
        }
    );
    transient.revision = next_revision(transient.revision);
    update();
    return true;
}

void EditPreviewTextureItem::finishTransientLiquify(const bool committed) {
    if (!state_->transient_liquify.active) {
        return;
    }
    if (!committed || state_->transient_liquify.base_source != state_->source) {
        clearTransientLiquify();
        return;
    }
    state_->transient_liquify.awaiting_authoritative_frame = true;
    state_->transient_liquify.revision = next_revision(state_->transient_liquify.revision);
    update();
}

void EditPreviewTextureItem::cancelTransientLiquify() {
    clearTransientLiquify();
}

EditPreviewTextureItem::FillMode EditPreviewTextureItem::fillMode() const noexcept {
    return state_->fill_mode;
}

void EditPreviewTextureItem::setFillMode(const FillMode fill_mode) {
    if (state_->fill_mode == fill_mode) {
        return;
    }
    state_->fill_mode = fill_mode;
    emit fillModeChanged();
    emit contentRectChanged();
    update();
}

QRectF EditPreviewTextureItem::contentRect() const noexcept {
    const QRectF bounds = boundingRect();
    if (state_->fill_mode == FillMode::Stretch || !state_->live_frame.has_value()
        || !state_->live_frame->dimensions.isValid() || state_->live_frame->dimensions.isEmpty()) {
        return bounds;
    }
    QSizeF scaled(state_->live_frame->dimensions);
    scaled.scale(bounds.size(), Qt::KeepAspectRatio);
    return QRectF(
        (bounds.width() - scaled.width()) / 2.0,
        (bounds.height() - scaled.height()) / 2.0,
        scaled.width(),
        scaled.height()
    );
}

QSGNode* EditPreviewTextureItem::updatePaintNode(
    QSGNode* const old_node,
    UpdatePaintNodeData* const update_data
) {
    static_cast<void>(update_data);
    auto* node = static_cast<EditPreviewTextureNode*>(old_node);
    if (!state_->live_frame.has_value()) {
        delete node;
        return nullptr;
    }
    const State::LiveFrame live = *state_->live_frame;
    const QRectF target_rect = contentRect();
    const std::uint64_t scene_graph_revision =
        state_->scene_graph_revision.load(std::memory_order_acquire);
    const EditPreviewTextureIdentity texture_identity = live.textureIdentity(scene_graph_revision);
    if (node == nullptr || node->texture_identity != texture_identity) {
        std::unique_ptr<QQuickTextureFactory> factory(makeEditPreviewTextureFactory(
            live.frame,
            live.presentation_binding,
            live.presentation_context
        ));
        QQuickWindow* const target_window = window();
        QSGTexture* const texture = factory != nullptr && target_window != nullptr
                                        ? factory->createTexture(target_window)
                                        : nullptr;
        if (texture == nullptr) {
            delete node;
            return nullptr;
        }
        if (node == nullptr) {
            node = new EditPreviewTextureNode();
        }
        node->replaceTexture(texture);
        node->texture_identity = texture_identity;
    }
    node->syncGeometry(target_rect, state_->transient_liquify);

    QMetaObject::invokeMethod(
        this,
        [this, texture_identity] {
            const std::uint64_t scene_graph_revision =
                state_->scene_graph_revision.load(std::memory_order_acquire);
            if (!state_->live_frame.has_value()
                || scene_graph_revision != texture_identity.scene_graph_revision
                || state_->live_frame->textureIdentity(scene_graph_revision) != texture_identity
                || state_->presented_generation == texture_identity.generation) {
                return;
            }
            state_->presented_generation = texture_identity.generation;
            emit presentedGenerationChanged();
        },
        Qt::QueuedConnection
    );
    return node;
}

void EditPreviewTextureItem::releaseResources() {
    revokePresentedTexture(advanceSceneGraphRevision());
    QQuickItem::releaseResources();
}

void EditPreviewTextureItem::geometryChange(
    const QRectF& new_geometry,
    const QRectF& old_geometry
) {
    QQuickItem::geometryChange(new_geometry, old_geometry);
    if (new_geometry.size() != old_geometry.size()) {
        emit contentRectChanged();
        update();
    }
}

void EditPreviewTextureItem::refreshSourceBinding() {
    const bool had_live_frame = state_->live_frame.has_value();
    const QString previous_fallback = state_->fallback_source;
    const QString previous_presented = state_->presented_generation;

    state_->source_binding_revision = next_revision(state_->source_binding_revision);
    state_->live_frame.reset();
    const auto parsed = parse_preview_source(state_->source);
    const auto owners = state_->presentation_registry != nullptr
                            ? state_->presentation_registry->owners()
                            : EditPreviewPresentationRegistry::Owners{};
    bool resolved_fallback_source = !parsed.has_value();
    if (parsed.has_value() && owners.available()) {
        const auto snapshot = owners.store->snapshot(parsed->slot, parsed->generation);
        const bool frame_available = snapshot.frame != nullptr;
        resolved_fallback_source =
            !snapshot.bytes.isEmpty() || (state_->live_admission_enabled && frame_available);
        if (state_->live_admission_enabled && frame_available && snapshot.row_stride_bytes > 0
            && snapshot.dimensions.isValid() && !snapshot.dimensions.isEmpty()
            && snapshot.frame->dimensions() == snapshot.dimensions
            && snapshot.frame->rowStrideBytes()
                   == static_cast<std::size_t>(snapshot.row_stride_bytes)) {
            state_->live_frame = State::LiveFrame{
                .frame = snapshot.frame,
                .presentation_binding = snapshot.presentation_binding,
                .presentation_context = owners.presentation_context,
                .dimensions = snapshot.dimensions,
                .source_identity = state_->source,
                .generation = QString::number(parsed->generation),
                .configuration_revision = owners.configuration_revision,
                .source_binding_revision = state_->source_binding_revision,
            };
        }
    }

    if (state_->live_frame.has_value()) {
        state_->presented_generation.clear();
    } else if (resolved_fallback_source) {
        state_->fallback_source = state_->source;
        state_->presented_generation.clear();
    } else {
        state_->presented_generation.clear();
    }
    if (had_live_frame != state_->live_frame.has_value()) {
        emit liveFrameAvailableChanged();
    }
    if (previous_fallback != state_->fallback_source) {
        emit fallbackSourceChanged();
    }
    if (previous_presented != state_->presented_generation) {
        emit presentedGenerationChanged();
    }
    if (!state_->live_frame.has_value() && state_->transient_liquify.active) {
        clearTransientLiquify();
    }
    emit contentRectChanged();
    update();
}

void EditPreviewTextureItem::bindWindowLifecycle(QQuickWindow* const next_window) {
    if (state_->observed_window == next_window) {
        return;
    }
    QObject::disconnect(state_->window_invalidated_connection);
    QObject::disconnect(state_->window_initialized_connection);
    state_->observed_window = next_window;
    state_->window_observation_revision = next_revision(state_->window_observation_revision);
    const std::uint64_t window_observation_revision = state_->window_observation_revision;
    revokePresentedTexture(advanceSceneGraphRevision());
    if (next_window == nullptr) {
        return;
    }

    state_->window_invalidated_connection = QObject::connect(
        next_window,
        &QQuickWindow::sceneGraphInvalidated,
        this,
        [this, window_observation_revision] {
            const std::uint64_t scene_graph_revision = advanceSceneGraphRevision();
            QMetaObject::invokeMethod(
                this,
                [this, window_observation_revision, scene_graph_revision] {
                    if (state_->window_observation_revision != window_observation_revision) {
                        return;
                    }
                    revokePresentedTexture(scene_graph_revision);
                },
                Qt::QueuedConnection
            );
        },
        Qt::DirectConnection
    );
    state_->window_initialized_connection = QObject::connect(
        next_window,
        &QQuickWindow::sceneGraphInitialized,
        this,
        [this, window_observation_revision] {
            QMetaObject::invokeMethod(
                this,
                [this, window_observation_revision] {
                    if (state_->window_observation_revision == window_observation_revision) {
                        update();
                    }
                },
                Qt::QueuedConnection
            );
        },
        Qt::DirectConnection
    );
}

void EditPreviewTextureItem::revokePresentedTexture(const std::uint64_t scene_graph_revision) {
    if (state_->scene_graph_revision.load(std::memory_order_acquire) != scene_graph_revision) {
        return;
    }
    state_->source_binding_revision = next_revision(state_->source_binding_revision);
    if (state_->live_frame.has_value()) {
        state_->live_frame->source_binding_revision = state_->source_binding_revision;
    }
    if (!state_->presented_generation.isEmpty()) {
        state_->presented_generation.clear();
        emit presentedGenerationChanged();
    }
    if (state_->live_frame.has_value() && window() != nullptr) {
        update();
    }
}

void EditPreviewTextureItem::clearTransientLiquify() {
    auto& transient = state_->transient_liquify;
    if (!transient.active && transient.points.empty() && !transient.awaiting_authoritative_frame) {
        return;
    }
    transient.points.clear();
    transient.base_source.clear();
    transient.radius = 0.0;
    transient.strength = 0.0;
    transient.hardness = 0.0;
    transient.identity = next_revision(transient.identity);
    transient.revision = next_revision(transient.revision);
    transient.active = false;
    transient.awaiting_authoritative_frame = false;
    emit transientLiquifyChanged();
    update();
}

std::uint64_t EditPreviewTextureItem::advanceSceneGraphRevision() noexcept {
    return advance_revision(state_->scene_graph_revision);
}
