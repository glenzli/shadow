#include "desktop_smoke/edit_preview_session.hpp"

#include "edit_controller.hpp"
#include "edit_preview_metal_texture_factory.hpp"
#include "edit_preview_provider.hpp"
#include "review_controller.hpp"

#include <QAbstractItemModel>
#include <QColorSpace>
#include <QCoreApplication>
#include <QDebug>
#include <QImage>
#include <QModelIndex>
#include <QObject>
#include <QPointer>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QSize>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>

namespace {

constexpr int CURRENT_PREVIEW_TIMEOUT_MS = 30'000;
constexpr int FULL_DETAIL_TIMEOUT_MS = 120'000;
constexpr int TERMINAL_SETTLE_MS = 50;
constexpr int FRAME_POLL_MS = 20;
constexpr std::uint32_t DETAIL_VIEWPORT_WIDTH = 1'280;
constexpr std::uint32_t DETAIL_VIEWPORT_HEIGHT = 960;
constexpr std::uint32_t DETAIL_TILE_SIDE = 512;
constexpr double DETAIL_CENTER = 0.5;

[[nodiscard]] QString imageProviderRequestId(const QString& source) {
    const QUrl url(source);
    QString id = url.path();
    if (id.startsWith(QLatin1Char('/'))) {
        id.remove(0, 1);
    }
    const QString query = url.query(QUrl::FullyEncoded);
    if (!query.isEmpty()) {
        id += QLatin1Char('?');
        id += query;
    }
    return id;
}

[[nodiscard]] QString previewGeneration(const QString& source) {
    return QUrlQuery(QUrl(source)).queryItemValue(QStringLiteral("generation"));
}

[[nodiscard]] bool validEditHistogram(const QVariantMap& histogram, const QString& source) {
    const QString source_generation = previewGeneration(source);
    if (!histogram.value(QStringLiteral("valid")).toBool()
        || histogram.value(QStringLiteral("updating")).toBool()
        || histogram.value(QStringLiteral("stale")).toBool()
        || histogram.value(QStringLiteral("version")).toString().isEmpty()
        || source_generation.isEmpty()
        || histogram.value(QStringLiteral("generation")).toString() != source_generation
        || histogram.value(QStringLiteral("targetGeneration")).toString() != source_generation) {
        return false;
    }
    const qulonglong pixel_count = histogram.value(QStringLiteral("pixelCount")).toULongLong();
    const qulonglong dimensions_count = histogram.value(QStringLiteral("width")).toULongLong()
                                        * histogram.value(QStringLiteral("height")).toULongLong();
    if (pixel_count == 0 || pixel_count != dimensions_count) {
        return false;
    }
    for (const auto& key : {
             QStringLiteral("red"),
             QStringLiteral("green"),
             QStringLiteral("blue"),
             QStringLiteral("luma"),
         }) {
        const QVariantList bins = histogram.value(key).toList();
        if (bins.size() != 256) {
            return false;
        }
        qulonglong sum = 0;
        for (const QVariant& bin : bins) {
            const qulonglong count = bin.toULongLong();
            if (count > pixel_count - sum) {
                return false;
            }
            sum += count;
        }
        if (sum != pixel_count) {
            return false;
        }
    }
    return histogram.value(QStringLiteral("belowZero")).toList().size() == 3
           && histogram.value(QStringLiteral("aboveOne")).toList().size() == 3
           && histogram.value(QStringLiteral("shadowClippedPixels")).toULongLong() <= pixel_count
           && histogram.value(QStringLiteral("highlightClippedPixels")).toULongLong()
                  <= pixel_count;
}

[[nodiscard]] QObject* precisionWorkspace(QQmlApplicationEngine& engine) {
    if (engine.rootObjects().isEmpty()) {
        return nullptr;
    }
    return engine.rootObjects().front()->findChild<QObject*>(QStringLiteral("precisionWorkspace"));
}

[[nodiscard]] QObject* precisionCanvas(QQmlApplicationEngine& engine) {
    auto* const workspace = precisionWorkspace(engine);
    return workspace == nullptr ? nullptr
                                : workspace->findChild<QObject*>(QStringLiteral("precisionCanvas"));
}

[[nodiscard]] QObject* liveEditedPreview(QQmlApplicationEngine& engine) {
    auto* const canvas = precisionCanvas(engine);
    return canvas == nullptr ? nullptr
                             : canvas->findChild<QObject*>(QStringLiteral("liveEditedPreview"));
}

[[nodiscard]] bool qmlPreviewIsReady(QQmlApplicationEngine& engine, const QString& source) {
    const auto* const workspace = precisionWorkspace(engine);
    if (workspace == nullptr || previewGeneration(source).isEmpty()) {
        return false;
    }
    if (QUrl(source).path().endsWith(QStringLiteral("/before"))) {
        return workspace->property("beforeFrameReady").toBool();
    }
    return workspace->property("readyPreviewGeneration").toString() == previewGeneration(source);
}

struct DetailAxisGrid final {
    std::uint32_t start = 0;
    std::uint32_t extent = 0;
};

[[nodiscard]] DetailAxisGrid
expectedDetailAxisGrid(const std::uint32_t full, const std::uint32_t requested) {
    const std::uint32_t span = std::min(full, requested);
    const std::uint32_t max_start = full - span;
    const double centered =
        DETAIL_CENTER * static_cast<double>(full) - static_cast<double>(span) / 2.0;
    const double rounded_start =
        std::clamp(std::round(centered), 0.0, static_cast<double>(max_start));
    const auto viewport_start = static_cast<std::uint32_t>(rounded_start);
    const std::uint32_t grid_start = viewport_start / DETAIL_TILE_SIDE * DETAIL_TILE_SIDE;
    const std::uint64_t viewport_end = static_cast<std::uint64_t>(viewport_start) + span;
    const std::uint64_t aligned_end =
        (viewport_end + DETAIL_TILE_SIDE - 1U) / DETAIL_TILE_SIDE * DETAIL_TILE_SIDE;
    const auto grid_end = static_cast<std::uint32_t>(std::min<std::uint64_t>(full, aligned_end));
    return {.start = grid_start, .extent = grid_end - grid_start};
}

class EditPreviewSession final : public std::enable_shared_from_this<EditPreviewSession> {
  public:
    static void start(
        QCoreApplication& application,
        QQmlApplicationEngine& engine,
        ReviewController& review,
        EditController& editor,
        EditPreviewProvider* const edit_preview_provider,
        DesktopSmoke::EditPreviewSessionOptions options
    ) {
        const auto session = std::shared_ptr<EditPreviewSession>(new EditPreviewSession(
            application,
            engine,
            review,
            editor,
            edit_preview_provider,
            options
        ));
        session->connectSignals();
    }

  private:
    enum class Stage : std::uint8_t {
        AwaitReviewItem,
        AwaitCurrentPreview,
        AwaitInteractivePreview,
        AwaitSettledAfterInteractive,
        AwaitSceneGraphInvalidation,
        AwaitSceneGraphReinitialization,
        AwaitInteractiveAfterRecreate,
        AwaitSettledAfterRecreate,
        AwaitBeforePreview,
        AwaitDualRoundtripInteractive,
        AwaitDualAdmissionRevocation,
        AwaitDualReturnPresentation,
        AwaitSettledAfterDualRoundtrip,
        AwaitFullDetail,
        Finished,
        Failed,
    };

    EditPreviewSession(
        QCoreApplication& application,
        QQmlApplicationEngine& engine,
        ReviewController& review,
        EditController& editor,
        EditPreviewProvider* const edit_preview_provider,
        DesktopSmoke::EditPreviewSessionOptions options
    ) :
        application_(application), engine_(engine), review_(review), editor_(editor),
        edit_preview_provider_(edit_preview_provider), options_(options) {}

    void connectSignals() {
        if (engine_.rootObjects().isEmpty()) {
            fail(QStringLiteral("the application has no loaded QML root"));
            return;
        }
        if (options_.request_full_detail && edit_preview_provider_ == nullptr) {
            fail(QStringLiteral("the full-detail image provider is unavailable"));
            return;
        }

        const auto self = shared_from_this();
        const auto evaluate = [self]() { self->evaluate(); };
        QObject::connect(&review_, &ReviewController::itemCountChanged, &application_, evaluate);
        QObject::connect(
            review_.reviewModel(),
            &QAbstractItemModel::modelReset,
            &application_,
            evaluate
        );
        QObject::connect(
            review_.reviewModel(),
            &QAbstractItemModel::rowsInserted,
            &application_,
            [self](const QModelIndex&, int, int) { self->evaluate(); }
        );
        QObject::connect(&editor_, &EditController::activeChanged, &application_, evaluate);
        QObject::connect(&editor_, &EditController::stateBusyChanged, &application_, evaluate);
        QObject::connect(&editor_, &EditController::previewSourceChanged, &application_, evaluate);
        QObject::connect(&editor_, &EditController::histogramChanged, &application_, evaluate);
        QObject::connect(
            &editor_,
            &EditController::beforePreviewSourceChanged,
            &application_,
            evaluate
        );
        QObject::connect(
            &editor_,
            &EditController::beforeHistogramChanged,
            &application_,
            evaluate
        );
        QObject::connect(
            &editor_,
            &EditController::detailRenderingChanged,
            &application_,
            evaluate
        );
        QObject::connect(&editor_, &EditController::detailGeometryChanged, &application_, evaluate);
        QObject::connect(&editor_, &EditController::detailTilesChanged, &application_, evaluate);
        QObject::connect(
            &editor_,
            &EditController::detailErrorTextChanged,
            &application_,
            evaluate
        );
        QObject::connect(
            &editor_,
            &EditController::fullResolutionStateChanged,
            &application_,
            evaluate
        );

        frame_poll_.setInterval(FRAME_POLL_MS);
        QObject::connect(&frame_poll_, &QTimer::timeout, &application_, evaluate);
        frame_poll_.start();

        deadline_.setSingleShot(true);
        deadline_.setInterval(
            options_.request_full_detail ? FULL_DETAIL_TIMEOUT_MS : CURRENT_PREVIEW_TIMEOUT_MS
        );
        QObject::connect(&deadline_, &QTimer::timeout, &application_, [self]() {
            self->fail(QStringLiteral("timed out before the acceptance terminal"));
        });
        deadline_.start();
        QTimer::singleShot(0, &application_, evaluate);
    }

    void evaluate() {
        switch (stage_) {
        case Stage::AwaitReviewItem:
            openFirstPhotoIfReady();
            break;
        case Stage::AwaitCurrentPreview:
            acceptCurrentPreviewIfReady();
            break;
        case Stage::AwaitInteractivePreview:
            acceptInteractivePreviewIfReady();
            break;
        case Stage::AwaitSettledAfterInteractive:
            acceptSettledAfterInteractiveIfReady();
            break;
        case Stage::AwaitSceneGraphInvalidation:
            acceptSceneGraphInvalidationIfReady();
            break;
        case Stage::AwaitSceneGraphReinitialization:
            acceptSceneGraphReinitializationIfReady();
            break;
        case Stage::AwaitInteractiveAfterRecreate:
            acceptInteractiveAfterRecreateIfReady();
            break;
        case Stage::AwaitSettledAfterRecreate:
            acceptSettledAfterRecreateIfReady();
            break;
        case Stage::AwaitBeforePreview:
            acceptBeforePreviewIfReady();
            break;
        case Stage::AwaitDualRoundtripInteractive:
            acceptDualRoundtripInteractiveIfReady();
            break;
        case Stage::AwaitDualAdmissionRevocation:
            acceptDualAdmissionRevocationIfReady();
            break;
        case Stage::AwaitDualReturnPresentation:
            acceptDualReturnPresentationIfReady();
            break;
        case Stage::AwaitSettledAfterDualRoundtrip:
            acceptSettledAfterDualRoundtripIfReady();
            break;
        case Stage::AwaitFullDetail:
            acceptFullDetailIfReady();
            break;
        case Stage::Finished:
        case Stage::Failed:
            break;
        }
    }

    void openFirstPhotoIfReady() {
        auto* const model = review_.reviewModel();
        if (model == nullptr || model->rowCount() == 0) {
            return;
        }
        const QModelIndex first = model->index(0, 0);
        const QString photo_id = model->data(first, ReviewModel::PhotoIdRole).toString();
        const QString representation_id =
            model->data(first, ReviewModel::RepresentationIdRole).toString();
        const QString source_path = model->data(first, ReviewModel::SourcePathRole).toString();
        const QString title = model->data(first, ReviewModel::TitleRole).toString();
        if (photo_id.isEmpty() || representation_id.isEmpty() || source_path.isEmpty()) {
            fail(QStringLiteral("the first Review row has an incomplete source identity"));
            return;
        }

        opened_photo_id_ = photo_id;
        opened_representation_id_ = representation_id;
        stage_ = Stage::AwaitCurrentPreview;
        editor_.openPhoto(photo_id, representation_id, source_path, title);
        engine_.rootObjects().front()->setProperty("workspaceIndex", 1);
        evaluate();
    }

    void acceptCurrentPreviewIfReady() {
        const QString source = editor_.previewSource();
        if (!editor_.active() || source.isEmpty()
            || !validEditHistogram(editor_.histogram(), source)
            || !qmlPreviewIsReady(engine_, source)) {
            return;
        }
        current_source_ = source;
        if (options_.transport_expectation != DesktopSmoke::EditPreviewTransportExpectation::None) {
            initial_settled_source_ = source;
            resetEditPreviewTextureTelemetry();
            stage_ = Stage::AwaitInteractivePreview;
            interactive_gesture_open_ = true;
            editor_.beginParameterEdit(QStringLiteral("exposure"));
            editor_.setExposureStops(editor_.exposureStops() + 0.1);
            evaluate();
            return;
        }
        continueAfterCurrentAccepted();
    }

    void acceptInteractivePreviewIfReady() {
        const QString source = editor_.previewSource();
        if (source.isEmpty() || source == initial_settled_source_
            || !qmlPreviewIsReady(engine_, source)) {
            return;
        }
        if (!transportReady(false)) {
            return;
        }

        first_transport_evidence_ = editPreviewTextureTelemetry();
        first_transport_epoch_ = first_transport_evidence_->last_epoch;
        interactive_source_ = source;
        stage_ = Stage::AwaitSettledAfterInteractive;
        interactive_gesture_open_ = false;
        editor_.endParameterEdit(QStringLiteral("exposure"));
        evaluate();
    }

    void acceptSettledAfterInteractiveIfReady() {
        const QString source = editor_.previewSource();
        if (source.isEmpty() || source == interactive_source_
            || !validEditHistogram(editor_.histogram(), source)
            || !qmlPreviewIsReady(engine_, source)) {
            return;
        }
        current_source_ = source;
        beginSceneGraphRecreation();
    }

    [[nodiscard]] bool transportReady(const bool require_recreated_epoch) {
        const auto transport = editPreviewTextureTelemetry();
        if (options_.transport_expectation
            == DesktopSmoke::EditPreviewTransportExpectation::MetalNative) {
            if (transport.host_materialization_calls != 0U
                || transport.host_materialization_bytes != 0U || transport.cpu_upload_bytes != 0U
                || transport.totalFallbackCount() != 0U) {
                fail(QStringLiteral("the Metal interactive frame left the zero-readback path"));
                return false;
            }
            if (transport.native_factory_count == 0U || transport.native_request_count == 0U
                || transport.native_import_count == 0U) {
                return false;
            }
            if (transport.native_factory_count != 1U || transport.native_request_count != 1U
                || transport.native_import_count != 1U) {
                fail(QStringLiteral(
                    "one active Metal preview surface imported more than "
                    "once in the same scene-graph epoch"
                ));
                return false;
            }
            if (transport.native_request_bytes == 0U || transport.native_import_bytes == 0U
                || transport.last_resource_id == 0U || transport.last_epoch == 0U
                || transport.active_native_frame_guards != 1U
                || transport.peak_native_frame_guards != 1U) {
                fail(QStringLiteral(
                    "the Metal import telemetry lacks its resource identity "
                    "or has an ambiguous retained frame owner"
                ));
                return false;
            }
            if (require_recreated_epoch && transport.last_epoch == first_transport_epoch_) {
                fail(QStringLiteral("the recreated scene graph reused the stale Metal epoch"));
                return false;
            }
        } else {
            if (transport.native_import_count != 0U || transport.native_import_bytes != 0U) {
                fail(QStringLiteral("the software renderer unexpectedly imported a Metal texture"));
                return false;
            }
            if (transport.host_materialization_calls == 0U
                || transport.host_materialization_bytes == 0U || transport.cpu_upload_bytes == 0U
                || transport.totalFallbackCount() == 0U) {
                return false;
            }
            if (transport.host_materialization_calls != 1U
                || transport.totalFallbackCount() != 1U) {
                fail(QStringLiteral(
                    "one active software preview surface materialized more "
                    "than once in the same scene-graph epoch"
                ));
                return false;
            }
        }
        return true;
    }

    void beginSceneGraphRecreation() {
        if (options_.transport_expectation == DesktopSmoke::EditPreviewTransportExpectation::None) {
            continueAfterCurrentAccepted();
            return;
        }
        scene_graph_window_ = qobject_cast<QQuickWindow*>(engine_.rootObjects().front());
        if (scene_graph_window_ == nullptr) {
            fail(QStringLiteral("the transport smoke root is not a QQuickWindow"));
            return;
        }
        scene_graph_window_->setPersistentGraphics(false);
        scene_graph_window_->setPersistentSceneGraph(false);
        scene_graph_window_->hide();
        scene_graph_window_->releaseResources();
        // Qt's software adaptation has no graphics-device epoch and keeps its
        // CPU scene graph initialized across hide/releaseResources(). Re-show
        // it immediately and require a new materialize/upload transaction.
        // The Metal path below must observe a real invalidation and a new
        // presentation epoch.
        if (options_.transport_expectation
            == DesktopSmoke::EditPreviewTransportExpectation::SoftwareFallback) {
            stage_ = Stage::AwaitSceneGraphReinitialization;
            scene_graph_window_->show();
            scene_graph_window_->update();
            evaluate();
            return;
        }
        stage_ = Stage::AwaitSceneGraphInvalidation;
        evaluate();
    }

    void acceptSceneGraphInvalidationIfReady() {
        if (scene_graph_window_ == nullptr) {
            fail(QStringLiteral("the transport smoke lost its scene graph window"));
            return;
        }
        if (scene_graph_window_->isSceneGraphInitialized()) {
            return;
        }
        stage_ = Stage::AwaitSceneGraphReinitialization;
        scene_graph_window_->show();
        scene_graph_window_->update();
        evaluate();
    }

    void acceptSceneGraphReinitializationIfReady() {
        if (scene_graph_window_ == nullptr) {
            fail(QStringLiteral("the transport smoke lost its recreated window"));
            return;
        }
        if (!scene_graph_window_->isSceneGraphInitialized()) {
            return;
        }
        if (!first_transport_evidence_.has_value()) {
            fail(QStringLiteral("the first transport phase has no accepted evidence"));
            return;
        }
        const auto before_recreated_transaction = editPreviewTextureTelemetry();
        if (before_recreated_transaction.native_factory_count
                != first_transport_evidence_->native_factory_count
            || before_recreated_transaction.native_request_count
                   != first_transport_evidence_->native_request_count
            || before_recreated_transaction.native_import_count
                   != first_transport_evidence_->native_import_count
            || before_recreated_transaction.host_materialization_calls
                   != first_transport_evidence_->host_materialization_calls
            || before_recreated_transaction.cpu_upload_bytes
                   != first_transport_evidence_->cpu_upload_bytes
            || before_recreated_transaction.totalFallbackCount()
                   != first_transport_evidence_->totalFallbackCount()) {
            fail(QStringLiteral(
                "the hidden or inactive preview surface repeated the first "
                "transport transaction before recreation"
            ));
            return;
        }
        recreated_settled_source_ = editor_.previewSource();
        resetEditPreviewTextureTelemetry();
        stage_ = Stage::AwaitInteractiveAfterRecreate;
        interactive_gesture_open_ = true;
        editor_.beginParameterEdit(QStringLiteral("exposure"));
        editor_.setExposureStops(editor_.exposureStops() + 0.1);
        evaluate();
    }

    void acceptInteractiveAfterRecreateIfReady() {
        const QString source = editor_.previewSource();
        if (source.isEmpty() || source == recreated_settled_source_
            || !qmlPreviewIsReady(engine_, source) || !transportReady(true)) {
            return;
        }
        recreated_transport_evidence_ = editPreviewTextureTelemetry();
        interactive_source_ = source;
        stage_ = Stage::AwaitSettledAfterRecreate;
        interactive_gesture_open_ = false;
        editor_.endParameterEdit(QStringLiteral("exposure"));
        evaluate();
    }

    void acceptSettledAfterRecreateIfReady() {
        const QString source = editor_.previewSource();
        if (source.isEmpty() || source == interactive_source_
            || !validEditHistogram(editor_.histogram(), source)
            || !qmlPreviewIsReady(engine_, source)) {
            return;
        }
        current_source_ = source;
        continueAfterCurrentAccepted();
    }

    void continueAfterCurrentAccepted() {
        if (options_.request_before) {
            stage_ = Stage::AwaitBeforePreview;
            editor_.requestBeforePreview();
            evaluate();
        } else if (options_.request_full_detail) {
            requestFullDetail();
        } else {
            succeed();
        }
    }

    void acceptBeforePreviewIfReady() {
        const QString source = editor_.beforePreviewSource();
        if (source.isEmpty() || !validEditHistogram(editor_.beforeHistogram(), source)
            || !qmlPreviewIsReady(engine_, source)) {
            return;
        }
        before_source_ = source;
        if (options_.transport_expectation != DesktopSmoke::EditPreviewTransportExpectation::None) {
            beginDualRoundtrip();
            return;
        }
        if (options_.request_full_detail) {
            requestFullDetail();
        } else {
            succeed();
        }
    }

    void beginDualRoundtrip() {
        auto* const workspace = precisionWorkspace(engine_);
        auto* const canvas = precisionCanvas(engine_);
        auto* const live_preview = liveEditedPreview(engine_);
        roundtrip_settled_source_ = editor_.previewSource();
        roundtrip_fallback_source_ = live_preview == nullptr
                                         ? QString{}
                                         : live_preview->property("fallbackSource").toString();
        if (workspace == nullptr || canvas == nullptr || live_preview == nullptr
            || roundtrip_settled_source_.isEmpty()
            || roundtrip_fallback_source_ != roundtrip_settled_source_
            || !workspace->property("previewFrameReady").toBool()) {
            fail(QStringLiteral("the dual roundtrip has no settled canvas/workspace fallback"));
            return;
        }

        resetEditPreviewTextureTelemetry();
        stage_ = Stage::AwaitDualRoundtripInteractive;
        interactive_gesture_open_ = true;
        editor_.beginParameterEdit(QStringLiteral("exposure"));
        editor_.setExposureStops(editor_.exposureStops() + 0.1);
        evaluate();
    }

    void acceptDualRoundtripInteractiveIfReady() {
        const QString source = editor_.previewSource();
        if (source.isEmpty() || source == roundtrip_settled_source_
            || !qmlPreviewIsReady(engine_, source) || !transportReady(false)) {
            return;
        }
        auto* const canvas = precisionCanvas(engine_);
        auto* const live_preview = liveEditedPreview(engine_);
        if (canvas == nullptr || live_preview == nullptr
            || !live_preview->property("liveAdmissionEnabled").toBool()
            || !live_preview->property("liveFrameAvailable").toBool()
            || live_preview->property("fallbackSource").toString() != roundtrip_fallback_source_) {
            fail(QStringLiteral(
                "the normal canvas did not present the roundtrip live frame "
                "above its settled fallback"
            ));
            return;
        }

        roundtrip_interactive_source_ = source;
        stage_ = Stage::AwaitDualAdmissionRevocation;
        canvas->setProperty("comparisonMode", canvas->property("comparisonSideBySide"));
        canvas->setProperty("comparisonActive", true);
        evaluate();
    }

    void acceptDualAdmissionRevocationIfReady() {
        auto* const workspace = precisionWorkspace(engine_);
        auto* const canvas = precisionCanvas(engine_);
        auto* const live_preview = liveEditedPreview(engine_);
        if (workspace == nullptr || canvas == nullptr || live_preview == nullptr) {
            fail(QStringLiteral("the dual roundtrip lost its QML presentation owners"));
            return;
        }
        if (!canvas->property("dualComparison").toBool()
            || live_preview->property("liveAdmissionEnabled").toBool()
            || live_preview->property("liveFrameAvailable").toBool()
            || !live_preview->property("presentedGeneration").toString().isEmpty()
            || workspace->property("previewFrameReady").toBool()
            || !workspace->property("readyPreviewGeneration").toString().isEmpty()) {
            return;
        }
        if (live_preview->property("fallbackSource").toString() != roundtrip_fallback_source_) {
            fail(QStringLiteral("dual admission revocation blanked the main settled fallback"));
            return;
        }

        stage_ = Stage::AwaitDualReturnPresentation;
        canvas->setProperty("comparisonActive", false);
        if (live_preview->property("fallbackSource").toString() != roundtrip_fallback_source_) {
            fail(QStringLiteral("leaving dual comparison lost the fallback before live import"));
            return;
        }
        evaluate();
    }

    void acceptDualReturnPresentationIfReady() {
        auto* const canvas = precisionCanvas(engine_);
        auto* const live_preview = liveEditedPreview(engine_);
        if (canvas == nullptr || live_preview == nullptr) {
            fail(QStringLiteral("the dual return lost its main presentation item"));
            return;
        }
        if (canvas->property("comparisonActive").toBool()
            || !live_preview->property("liveAdmissionEnabled").toBool()
            || !live_preview->property("liveFrameAvailable").toBool()
            || live_preview->property("fallbackSource").toString() != roundtrip_fallback_source_
            || !qmlPreviewIsReady(engine_, roundtrip_interactive_source_)) {
            return;
        }

        stage_ = Stage::AwaitSettledAfterDualRoundtrip;
        interactive_gesture_open_ = false;
        editor_.endParameterEdit(QStringLiteral("exposure"));
        evaluate();
    }

    void acceptSettledAfterDualRoundtripIfReady() {
        const QString source = editor_.previewSource();
        if (source.isEmpty() || source == roundtrip_interactive_source_
            || !validEditHistogram(editor_.histogram(), source)
            || !qmlPreviewIsReady(engine_, source)) {
            return;
        }
        current_source_ = source;
        if (options_.request_full_detail) {
            requestFullDetail();
        } else {
            succeed();
        }
    }

    void requestFullDetail() {
        stage_ = Stage::AwaitFullDetail;
        editor_.requestDetailViewport(
            DETAIL_CENTER,
            DETAIL_CENTER,
            static_cast<int>(DETAIL_VIEWPORT_WIDTH),
            static_cast<int>(DETAIL_VIEWPORT_HEIGHT)
        );
        evaluate();
    }

    void acceptFullDetailIfReady() {
        if (!editor_.detailErrorText().isEmpty()) {
            fail(
                QStringLiteral("the full-detail renderer failed: %1").arg(editor_.detailErrorText())
            );
            return;
        }
        const QVariantList tiles = editor_.detailTiles();
        if (tiles.isEmpty() || editor_.detailRendering() || editor_.fullResolutionPreparing()
            || !editor_.fullResolutionReady()) {
            return;
        }
        if (tiles.size() != 1 || !editor_.detailMode() || editor_.detailFullWidth() == 0
            || editor_.detailFullHeight() == 0 || editor_.detailRetainedBytes() == 0
            || editor_.fullResolutionRetainedBytes() != editor_.detailRetainedBytes()) {
            fail(QStringLiteral("the detail publication is not one settled atomic viewport"));
            return;
        }

        const QVariantMap viewport = tiles.front().toMap();
        const std::uint32_t x = viewport.value(QStringLiteral("x")).toUInt();
        const std::uint32_t y = viewport.value(QStringLiteral("y")).toUInt();
        const std::uint32_t width = viewport.value(QStringLiteral("width")).toUInt();
        const std::uint32_t height = viewport.value(QStringLiteral("height")).toUInt();
        const QString source = viewport.value(QStringLiteral("source")).toString();
        const DetailAxisGrid expected_x =
            expectedDetailAxisGrid(editor_.detailFullWidth(), DETAIL_VIEWPORT_WIDTH);
        const DetailAxisGrid expected_y =
            expectedDetailAxisGrid(editor_.detailFullHeight(), DETAIL_VIEWPORT_HEIGHT);
        if (x != expected_x.start || y != expected_y.start || width != expected_x.extent
            || height != expected_y.extent || source.isEmpty()) {
            fail(QStringLiteral(
                "the atomic detail viewport does not cover the "
                "complete requested grid"
            ));
            return;
        }

        const QUrl source_url(source);
        const QUrlQuery source_query(source_url);
        bool photo_ok = false;
        bool recipe_ok = false;
        bool viewport_ok = false;
        const qulonglong photo_generation =
            source_query.queryItemValue(QStringLiteral("photo")).toULongLong(&photo_ok);
        const qulonglong recipe_generation =
            source_query.queryItemValue(QStringLiteral("recipe")).toULongLong(&recipe_ok);
        const qulonglong viewport_generation =
            source_query.queryItemValue(QStringLiteral("viewport")).toULongLong(&viewport_ok);
        const QString expected_ticket = QStringLiteral("/detail/viewport-%1-%2").arg(x).arg(y);
        if (source_url.scheme() != QStringLiteral("image")
            || source_url.host() != QStringLiteral("shadow-edit")
            || source_url.path() != expected_ticket || !photo_ok || !recipe_ok || !viewport_ok
            || photo_generation == 0 || recipe_generation == 0 || viewport_generation == 0) {
            fail(QStringLiteral("the detail image URL lacks its exact generation contract"));
            return;
        }

        QSize decoded_size;
        const QImage image =
            edit_preview_provider_->requestImage(imageProviderRequestId(source), &decoded_size, {});
        const QSize expected_size(static_cast<int>(width), static_cast<int>(height));
        if (image.isNull() || decoded_size != expected_size || image.size() != expected_size
            || image.colorSpace() != QColorSpace(QColorSpace::SRgb)
            || image.format() != QImage::Format_RGB888) {
            fail(QStringLiteral(
                "the complete detail viewport could not be read back "
                "as display-sRGB RGB8"
            ));
            return;
        }
        detail_source_ = source;
        succeed();
    }

    void succeed() {
        if (stage_ == Stage::Finished || stage_ == Stage::Failed) {
            return;
        }
        stage_ = Stage::Finished;
        frame_poll_.stop();
        deadline_.stop();
        qInfo().noquote() << "Edit preview smoke passed:"
                          << "photo" << opened_photo_id_ << "representation"
                          << opened_representation_id_ << "current" << current_source_ << "before"
                          << (options_.request_before ? before_source_
                                                      : QStringLiteral("<not requested>"))
                          << "detail"
                          << (options_.request_full_detail ? detail_source_
                                                           : QStringLiteral("<not requested>"))
                          << "texture transport" << transportDiagnostic();
        QTimer::singleShot(TERMINAL_SETTLE_MS, &application_, &QCoreApplication::quit);
    }

    void fail(const QString& reason) {
        if (stage_ == Stage::Finished || stage_ == Stage::Failed) {
            return;
        }
        const Stage failed_stage = stage_;
        stage_ = Stage::Failed;
        if (interactive_gesture_open_) {
            interactive_gesture_open_ = false;
            editor_.endParameterEdit(QStringLiteral("exposure"));
        }
        frame_poll_.stop();
        deadline_.stop();
        const QString current_source = editor_.previewSource();
        const QString before_source = editor_.beforePreviewSource();
        const auto* const workspace = precisionWorkspace(engine_);
        qCritical().noquote()
            << "Edit preview smoke failed:" << reason << "stage" << stageName(failed_stage)
            << "rows" << (review_.reviewModel() == nullptr ? -1 : review_.reviewModel()->rowCount())
            << "scanning" << review_.scanning() << "refreshing" << review_.refreshing() << "active"
            << editor_.active() << "state busy" << editor_.stateBusy() << "rendering"
            << editor_.rendering() << "current source" << current_source
            << "current histogram valid" << validEditHistogram(editor_.histogram(), current_source)
            << "before source" << before_source << "before histogram valid"
            << validEditHistogram(editor_.beforeHistogram(), before_source) << "QML generation"
            << (workspace == nullptr ? QStringLiteral("<missing workspace>")
                                     : workspace->property("readyPreviewGeneration").toString())
            << "QML before ready"
            << (workspace != nullptr && workspace->property("beforeFrameReady").toBool())
            << "detail mode" << editor_.detailMode() << "detail rendering"
            << editor_.detailRendering() << "full resolution preparing"
            << editor_.fullResolutionPreparing() << "full resolution ready"
            << editor_.fullResolutionReady() << "full resolution retained bytes"
            << editor_.fullResolutionRetainedBytes() << "detail geometry"
            << editor_.detailFullWidth() << "x" << editor_.detailFullHeight() << "tiles"
            << editor_.detailTiles().size() << "retained bytes" << editor_.detailRetainedBytes()
            << "detail error" << editor_.detailErrorText() << "status" << editor_.statusText()
            << "texture transport" << transportDiagnostic();
        application_.exit(EXIT_FAILURE);
    }

    [[nodiscard]] static QString stageName(const Stage stage) {
        switch (stage) {
        case Stage::AwaitReviewItem:
            return QStringLiteral("await-review-item");
        case Stage::AwaitCurrentPreview:
            return QStringLiteral("await-current-preview");
        case Stage::AwaitInteractivePreview:
            return QStringLiteral("await-interactive-preview");
        case Stage::AwaitSettledAfterInteractive:
            return QStringLiteral("await-settled-after-interactive");
        case Stage::AwaitSceneGraphInvalidation:
            return QStringLiteral("await-scene-graph-invalidation");
        case Stage::AwaitSceneGraphReinitialization:
            return QStringLiteral("await-scene-graph-reinitialization");
        case Stage::AwaitInteractiveAfterRecreate:
            return QStringLiteral("await-interactive-after-recreate");
        case Stage::AwaitSettledAfterRecreate:
            return QStringLiteral("await-settled-after-recreate");
        case Stage::AwaitBeforePreview:
            return QStringLiteral("await-before-preview");
        case Stage::AwaitDualRoundtripInteractive:
            return QStringLiteral("await-dual-roundtrip-interactive");
        case Stage::AwaitDualAdmissionRevocation:
            return QStringLiteral("await-dual-admission-revocation");
        case Stage::AwaitDualReturnPresentation:
            return QStringLiteral("await-dual-return-presentation");
        case Stage::AwaitSettledAfterDualRoundtrip:
            return QStringLiteral("await-settled-after-dual-roundtrip");
        case Stage::AwaitFullDetail:
            return QStringLiteral("await-full-detail");
        case Stage::Finished:
            return QStringLiteral("finished");
        case Stage::Failed:
            return QStringLiteral("failed");
        }
        return QStringLiteral("unknown");
    }

    [[nodiscard]] static QString
    transportSnapshotDiagnostic(const EditPreviewTextureTelemetrySnapshot& value) {
        return QStringLiteral(
                   "factory=%1 request=%2/%3 import=%4/%5 "
                   "guards=%6/%7 materialize=%8/%9 upload=%10 "
                   "fallback=%11 resource=%12 epoch=%13"
        )
            .arg(value.native_factory_count)
            .arg(value.native_request_count)
            .arg(value.native_request_bytes)
            .arg(value.native_import_count)
            .arg(value.native_import_bytes)
            .arg(value.active_native_frame_guards)
            .arg(value.peak_native_frame_guards)
            .arg(value.host_materialization_calls)
            .arg(value.host_materialization_bytes)
            .arg(value.cpu_upload_bytes)
            .arg(value.totalFallbackCount())
            .arg(value.last_resource_id)
            .arg(value.last_epoch);
    }

    [[nodiscard]] QString transportDiagnostic() const {
        const QString current = transportSnapshotDiagnostic(editPreviewTextureTelemetry());
        if (!first_transport_evidence_.has_value() && !recreated_transport_evidence_.has_value()) {
            return current;
        }
        return QStringLiteral("first=[%1] recreated=[%2] current=[%3]")
            .arg(
                first_transport_evidence_.has_value()
                    ? transportSnapshotDiagnostic(*first_transport_evidence_)
                    : QStringLiteral("<missing>")
            )
            .arg(
                recreated_transport_evidence_.has_value()
                    ? transportSnapshotDiagnostic(*recreated_transport_evidence_)
                    : QStringLiteral("<missing>")
            )
            .arg(current);
    }

    QCoreApplication& application_;
    QQmlApplicationEngine& engine_;
    ReviewController& review_;
    EditController& editor_;
    EditPreviewProvider* edit_preview_provider_ = nullptr;
    DesktopSmoke::EditPreviewSessionOptions options_;
    QTimer frame_poll_;
    QTimer deadline_;
    Stage stage_ = Stage::AwaitReviewItem;
    QString opened_photo_id_;
    QString opened_representation_id_;
    QString current_source_;
    QString initial_settled_source_;
    QString recreated_settled_source_;
    QString interactive_source_;
    QString before_source_;
    QString roundtrip_settled_source_;
    QString roundtrip_fallback_source_;
    QString roundtrip_interactive_source_;
    QString detail_source_;
    QPointer<QQuickWindow> scene_graph_window_;
    std::optional<EditPreviewTextureTelemetrySnapshot> first_transport_evidence_;
    std::optional<EditPreviewTextureTelemetrySnapshot> recreated_transport_evidence_;
    std::uint64_t first_transport_epoch_ = 0U;
    bool interactive_gesture_open_ = false;
};

} // namespace

void DesktopSmoke::startEditPreviewSession(
    QCoreApplication& application,
    QQmlApplicationEngine& engine,
    ReviewController& review,
    EditController& editor,
    EditPreviewProvider* const edit_preview_provider,
    const EditPreviewSessionOptions options
) {
    EditPreviewSession::start(application, engine, review, editor, edit_preview_provider, options);
}
