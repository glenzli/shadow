#include "desktop_backend.hpp"
#include "edit_controller.hpp"
#include "edit_preview_provider.hpp"
#include "review_controller.hpp"
#include "thumbnail_provider.hpp"

#include <QDebug>
#include <QColorSpace>
#include <QDir>
#include <QGuiApplication>
#include <QImage>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QSize>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QVector>

#include <cmath>
#include <cstdint>
#include <memory>

namespace {

[[nodiscard]] QString image_provider_request_id(const QString& source) {
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

class AdjustmentStackSmoke final
    : public std::enable_shared_from_this<AdjustmentStackSmoke> {
public:
    static void start(
        QCoreApplication& application,
        ReviewController& review,
        EditController& editor
    ) {
        const auto smoke = std::shared_ptr<AdjustmentStackSmoke>(
            new AdjustmentStackSmoke(application, review, editor)
        );
        smoke->connectSignals();
    }

private:
    enum class Stage : std::uint8_t {
        AwaitInitialPreview,
        AwaitAdjustedPreview,
        Saving,
        Reopening,
        Verifying,
        Finished,
        Failed,
    };

    AdjustmentStackSmoke(
        QCoreApplication& application,
        ReviewController& review,
        EditController& editor
    )
        : application_(application), review_(review), editor_(editor) {}

    static QString layerId(const QVariantList& layers, const qsizetype index) {
        return layers.at(index)
            .toMap()
            .value(QStringLiteral("layerId"))
            .toString();
    }

    static bool layerEnabled(const QVariantList& layers, const qsizetype index) {
        return layers.at(index)
            .toMap()
            .value(QStringLiteral("enabled"))
            .toBool();
    }

    void connectSignals() {
        const auto self = shared_from_this();
        QObject::connect(
            &editor_,
            &EditController::previewSourceChanged,
            &application_,
            [self]() { self->previewChanged(); }
        );
        QObject::connect(
            &editor_,
            &EditController::stateBusyChanged,
            &application_,
            [self]() { self->stateBusyChanged(); }
        );
        QTimer::singleShot(30'000, &application_, [self]() {
            if (self->stage_ != Stage::Finished) {
                self->fail(QStringLiteral("timed out after 30 seconds"));
            }
        });
    }

    void previewChanged() {
        if (editor_.previewSource().isEmpty()) {
            return;
        }
        if (stage_ == Stage::AwaitInitialPreview) {
            buildStack();
        } else if (stage_ == Stage::AwaitAdjustedPreview) {
            stage_ = Stage::Saving;
            const auto self = shared_from_this();
            QTimer::singleShot(0, &editor_, [self]() { self->save(); });
        } else if (stage_ == Stage::Reopening) {
            stage_ = Stage::Verifying;
            const auto self = shared_from_this();
            QTimer::singleShot(0, &editor_, [self]() { self->verifyReopen(); });
        }
    }

    void stateBusyChanged() {
        if (editor_.stateBusy()) {
            return;
        }
        const auto self = shared_from_this();
        if (stage_ == Stage::Saving) {
            QTimer::singleShot(0, &editor_, [self]() { self->finishSave(); });
        } else if (stage_ == Stage::Reopening) {
            QTimer::singleShot(0, &editor_, [self]() {
                if (self->stage_ == Stage::Reopening
                    && (!self->editor_.active()
                        || self->editor_.statusText().startsWith(
                            QStringLiteral("Version operation failed")
                        ))) {
                    self->fail(QStringLiteral("the saved photo could not be reloaded"));
                }
            });
        }
    }

    void buildStack() {
        auto* const model = review_.reviewModel();
        if (!expect(model->rowCount() > 0, QStringLiteral("Review item disappeared"))) {
            return;
        }
        const QModelIndex first = model->index(0, 0);
        photo_id_ = model->data(first, ReviewModel::PhotoIdRole).toString();
        representation_id_ = model->data(
            first,
            ReviewModel::RepresentationIdRole
        ).toString();
        source_path_ = model->data(first, ReviewModel::SourcePathRole).toString();
        title_ = model->data(first, ReviewModel::TitleRole).toString();

        const QVariantList initial_layers = editor_.layers();
        if (!expect(
                initial_layers.size() == 1,
                QStringLiteral("expected one initial layer, found %1")
                    .arg(initial_layers.size())
            )) {
            return;
        }
        const QString initial_id = layerId(initial_layers, 0);
        if (!expect(!initial_id.isEmpty(), QStringLiteral("initial layer has no ID"))) {
            return;
        }

        editor_.addLayer();
        if (!expect(editor_.layers().size() == 2, QStringLiteral("add layer failed"))) {
            return;
        }
        const QString added_id = editor_.selectedLayerId();
        editor_.setExposureStops(expected_exposure_);
        editor_.duplicateSelectedLayer();
        if (!expect(
                editor_.layers().size() == 3,
                QStringLiteral("duplicate layer failed")
            )) {
            return;
        }
        const QString duplicate_id = editor_.selectedLayerId();
        if (!expect(
                !added_id.isEmpty() && !duplicate_id.isEmpty()
                    && added_id != initial_id && duplicate_id != initial_id
                    && duplicate_id != added_id,
                QStringLiteral("new layers did not receive unique stable IDs")
            )) {
            return;
        }

        editor_.moveSelectedLayer(0);
        editor_.setLayerEnabled(false);
        expected_layer_ids_ = {duplicate_id, initial_id, added_id};
        const QVariantList adjusted_layers = editor_.layers();
        if (!expect(
                adjusted_layers.size() == 3
                    && layerId(adjusted_layers, 0) == duplicate_id
                    && layerId(adjusted_layers, 1) == initial_id
                    && layerId(adjusted_layers, 2) == added_id
                    && !layerEnabled(adjusted_layers, 0),
                QStringLiteral("reorder or bypass failed")
            )) {
            return;
        }
        stage_ = Stage::AwaitAdjustedPreview;
    }

    void save() {
        if (stage_ != Stage::Saving) {
            return;
        }
        if (!expect(
                !editor_.rendering() && !editor_.stateBusy(),
                QStringLiteral("final preview did not settle before save")
            )) {
            return;
        }
        editor_.saveVersion(QStringLiteral("Adjustment Stack Smoke"));
        expect(editor_.stateBusy(), QStringLiteral("version save did not start"));
    }

    void finishSave() {
        if (stage_ != Stage::Saving) {
            return;
        }
        if (!expect(
                !editor_.stateBusy() && !editor_.dirty()
                    && !editor_.statusText().startsWith(
                        QStringLiteral("Version operation failed")
                    ),
                QStringLiteral("immutable version was not saved")
            )) {
            return;
        }
        editor_.closePhoto();
        if (!expect(!editor_.active(), QStringLiteral("saved photo could not close"))) {
            return;
        }
        stage_ = Stage::Reopening;
        editor_.openPhoto(photo_id_, representation_id_, source_path_, title_);
        expect(
            editor_.active() && editor_.stateBusy(),
            QStringLiteral("saved photo could not reopen")
        );
    }

    void verifyReopen() {
        if (stage_ != Stage::Verifying) {
            return;
        }
        const QVariantList layers = editor_.layers();
        if (!expect(
                !editor_.stateBusy() && !editor_.rendering() && !editor_.dirty()
                    && layers.size() == expected_layer_ids_.size(),
                QStringLiteral("reopened stack was not clean and settled")
            )) {
            return;
        }
        for (qsizetype index = 0; index < expected_layer_ids_.size(); ++index) {
            if (!expect(
                    layerId(layers, index) == expected_layer_ids_.at(index),
                    QStringLiteral("stable layer order changed after reopen")
                )) {
                return;
            }
        }
        if (!expect(
                !layerEnabled(layers, 0) && layerEnabled(layers, 2),
                QStringLiteral("bypass state changed after reopen")
            )) {
            return;
        }

        editor_.selectLayer(0);
        const bool duplicate_parameter_ok =
            editor_.selectedLayerId() == expected_layer_ids_.at(0)
            && std::abs(editor_.exposureStops() - expected_exposure_) < 1.0e-9;
        editor_.selectLayer(2);
        const bool source_parameter_ok =
            editor_.selectedLayerId() == expected_layer_ids_.at(2)
            && std::abs(editor_.exposureStops() - expected_exposure_) < 1.0e-9;
        if (!expect(
                duplicate_parameter_ok && source_parameter_ok,
                QStringLiteral("layer parameters changed after reopen")
            )) {
            return;
        }

        stage_ = Stage::Finished;
        qInfo() << "Adjustment Stack smoke passed with three persisted layers";
        QTimer::singleShot(50, &application_, &QCoreApplication::quit);
    }

    bool expect(const bool condition, const QString& reason) {
        if (!condition) {
            fail(reason);
        }
        return condition;
    }

    void fail(const QString& reason) {
        if (stage_ == Stage::Finished || stage_ == Stage::Failed) {
            return;
        }
        stage_ = Stage::Failed;
        qCritical().noquote() << "Adjustment Stack smoke failed:" << reason;
        application_.exit(EXIT_FAILURE);
    }

    QCoreApplication& application_;
    ReviewController& review_;
    EditController& editor_;
    Stage stage_ = Stage::AwaitInitialPreview;
    QString photo_id_;
    QString representation_id_;
    QString source_path_;
    QString title_;
    QVector<QString> expected_layer_ids_;
    const double expected_exposure_ = 0.75;
};

} // namespace

int main(int argc, char* argv[]) {
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QGuiApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("Shadow"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("shadow.dev"));
    QCoreApplication::setApplicationName(QStringLiteral("Shadow"));

    QString application_data = qEnvironmentVariable("SHADOW_DESKTOP_DATA_ROOT");
    if (application_data.isEmpty()) {
        application_data =
            QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    }
    QDir().mkpath(application_data);
    const QString catalog_path = QDir(application_data).filePath(QStringLiteral("catalog.sqlite"));
    const QString cache_root = QDir(application_data).filePath(QStringLiteral("cache"));

    std::shared_ptr<DesktopBackend> backend;
    try {
        backend = std::make_shared<DesktopBackend>(catalog_path, cache_root);
    } catch (const std::exception& error) {
        qCritical() << "Cannot start Shadow's local backend:" << error.what();
        return EXIT_FAILURE;
    }
    ReviewController controller(backend);
    auto edit_preview_store = std::make_shared<EditPreviewStore>();
    EditController editor(backend, edit_preview_store);
    QQmlApplicationEngine engine;
    auto* const thumbnail_provider = new ThumbnailProvider(
        backend,
        controller.reviewModel()
    );
    engine.addImageProvider(
        QStringLiteral("shadow"),
        thumbnail_provider
    );
    auto* const edit_preview_provider = new EditPreviewProvider(edit_preview_store);
    engine.addImageProvider(
        QStringLiteral("shadow-edit"),
        edit_preview_provider
    );
    engine.setInitialProperties({
        {QStringLiteral("controller"), QVariant::fromValue(&controller)},
        {QStringLiteral("editor"), QVariant::fromValue(&editor)},
    });
    engine.loadFromModule("Shadow.App", "Main");
    if (engine.rootObjects().isEmpty()) {
        return EXIT_FAILURE;
    }
    const QString initial_folder = qEnvironmentVariable("SHADOW_DESKTOP_SCAN_FOLDER");
    const bool open_first_edit = qEnvironmentVariableIsSet("SHADOW_DESKTOP_OPEN_FIRST_EDIT");
    const bool record_first_comparison = qEnvironmentVariableIsSet(
        "SHADOW_DESKTOP_RECORD_FIRST_COMPARISON"
    );
    const bool forget_recorded_comparison = qEnvironmentVariableIsSet(
        "SHADOW_DESKTOP_FORGET_RECORDED_COMPARISON"
    );
    const bool set_first_decision = qEnvironmentVariableIsSet(
        "SHADOW_DESKTOP_SET_FIRST_DECISION"
    );
    const bool undo_first_decision = qEnvironmentVariableIsSet(
        "SHADOW_DESKTOP_UNDO_FIRST_DECISION"
    );
    const bool request_before = qEnvironmentVariableIsSet(
        "SHADOW_DESKTOP_REQUEST_BEFORE"
    );
    const bool adjustment_stack_smoke = qEnvironmentVariableIsSet(
        "SHADOW_DESKTOP_ADJUSTMENT_STACK_SMOKE"
    );
    const bool full_detail_smoke = qEnvironmentVariableIsSet(
        "SHADOW_DESKTOP_FULL_DETAIL_SMOKE"
    );
    if (open_first_edit && !record_first_comparison && !set_first_decision) {
        QObject::connect(
            &controller,
            &ReviewController::itemCountChanged,
            &application,
            [&controller, &editor]() {
                auto* model = controller.reviewModel();
                if (editor.active() || model->rowCount() == 0) {
                    return;
                }
                const QModelIndex first = model->index(0, 0);
                editor.openPhoto(
                    model->data(first, ReviewModel::PhotoIdRole).toString(),
                    model->data(first, ReviewModel::RepresentationIdRole).toString(),
                    model->data(first, ReviewModel::SourcePathRole).toString(),
                    model->data(first, ReviewModel::TitleRole).toString()
                );
            }
        );
    }
    if (record_first_comparison) {
        QObject::connect(
            &controller,
            &ReviewController::itemCountChanged,
            &application,
            [&controller, thumbnail_provider]() {
                auto* model = controller.reviewModel();
                if (model->rowCount() < 2 || controller.comparisonBusy()
                    || controller.sessionEvidenceCount() > 0) {
                    return;
                }
                const QModelIndex left = model->index(0, 0);
                const QModelIndex right = model->index(1, 0);
                const QVariantMap presentation = controller.prepareComparison(
                    model->data(left, ReviewModel::VisualHandleRole).toString(),
                    model->data(right, ReviewModel::VisualHandleRole).toString()
                );
                const QString presentation_id =
                    presentation.value(QStringLiteral("presentationId")).toString();
                const QString left_ticket =
                    presentation.value(QStringLiteral("leftRequestTicket")).toString();
                const QString right_ticket =
                    presentation.value(QStringLiteral("rightRequestTicket")).toString();
                if (presentation_id.isEmpty() || left_ticket.isEmpty()
                    || right_ticket.isEmpty()) {
                    return;
                }
                const QSize requested_size(1'280, 960);
                QSize left_size;
                QSize right_size;
                const QImage left_image = thumbnail_provider->requestImage(
                    image_provider_request_id(
                        presentation.value(QStringLiteral("leftSource")).toString()
                    ),
                    &left_size,
                    requested_size
                );
                const QImage right_image = thumbnail_provider->requestImage(
                    image_provider_request_id(
                        presentation.value(QStringLiteral("rightSource")).toString()
                    ),
                    &right_size,
                    requested_size
                );
                if (left_image.isNull() || right_image.isNull()
                    || !controller.confirmComparisonReady(
                        presentation_id,
                        left_ticket,
                        right_ticket
                    )) {
                    return;
                }
                controller.recordComparison(presentation_id, 0);
            }
        );
    }
    if (set_first_decision && !record_first_comparison) {
        auto decision_requested = std::make_shared<bool>(false);
        QObject::connect(
            &controller,
            &ReviewController::itemCountChanged,
            &application,
            [&controller, decision_requested]() {
                auto* model = controller.reviewModel();
                if (*decision_requested || model->rowCount() == 0
                    || controller.decisionBusy()) {
                    return;
                }
                *decision_requested = true;
                const QModelIndex first = model->index(0, 0);
                controller.setPhotoFlag(
                    model->data(first, ReviewModel::PhotoIdRole).toString(),
                    QStringLiteral("picked")
                );
            }
        );
    }
    if (!initial_folder.isEmpty()) {
        controller.scanFolder(QUrl::fromLocalFile(initial_folder));
    }
    if (qEnvironmentVariableIsSet("SHADOW_DESKTOP_SMOKE_TEST")) {
        if (record_first_comparison) {
            auto comparison_succeeded = std::make_shared<bool>(false);
            QObject::connect(
                &controller,
                &ReviewController::comparisonRecorded,
                &application,
                [&application, &controller, forget_recorded_comparison,
                 comparison_succeeded]() {
                    if (forget_recorded_comparison) {
                        controller.undoLastComparison();
                    } else {
                        *comparison_succeeded = true;
                        QTimer::singleShot(50, &application, &QCoreApplication::quit);
                    }
                }
            );
            QObject::connect(
                &controller,
                &ReviewController::comparisonForgotten,
                &application,
                [&application, comparison_succeeded]() {
                    *comparison_succeeded = true;
                    QTimer::singleShot(50, &application, &QCoreApplication::quit);
                }
            );
            QTimer::singleShot(
                30'000,
                &application,
                [&application, comparison_succeeded]() {
                    application.exit(*comparison_succeeded ? EXIT_SUCCESS : EXIT_FAILURE);
                }
            );
        } else if (set_first_decision) {
            auto decision_succeeded = std::make_shared<bool>(false);
            QObject::connect(
                &controller,
                &ReviewController::decisionChanged,
                &application,
                [&application, &controller, undo_first_decision, decision_succeeded](
                    const QString&,
                    const qulonglong,
                    const QString& flag,
                    const int
                ) {
                    if (flag != QStringLiteral("picked")
                        || !controller.canUndoDecision()) {
                        return;
                    }
                    if (undo_first_decision) {
                        QTimer::singleShot(
                            0,
                            &controller,
                            &ReviewController::undoLastDecision
                        );
                    } else {
                        *decision_succeeded = true;
                        QTimer::singleShot(50, &application, &QCoreApplication::quit);
                    }
                }
            );
            QObject::connect(
                &controller,
                &ReviewController::decisionUndone,
                &application,
                [&application, decision_succeeded]() {
                    *decision_succeeded = true;
                    QTimer::singleShot(50, &application, &QCoreApplication::quit);
                }
            );
            QTimer::singleShot(
                30'000,
                &application,
                [&application, decision_succeeded]() {
                    application.exit(*decision_succeeded ? EXIT_SUCCESS : EXIT_FAILURE);
                }
            );
        } else if (open_first_edit && full_detail_smoke) {
            auto requested = std::make_shared<bool>(false);
            auto succeeded = std::make_shared<bool>(false);
            QObject::connect(
                &editor,
                &EditController::previewSourceChanged,
                &application,
                [&editor, requested]() {
                    if (*requested || editor.previewSource().isEmpty()) {
                        return;
                    }
                    *requested = true;
                    editor.requestDetailViewport(0.5, 0.5, 1'280, 960);
                }
            );
            QObject::connect(
                &editor,
                &EditController::detailTilesChanged,
                &application,
                [&application, &editor, edit_preview_provider, succeeded]() {
                    const QVariantList tiles = editor.detailTiles();
                    if (tiles.isEmpty() || editor.detailFullWidth() == 0
                        || editor.detailFullHeight() == 0
                        || editor.detailRetainedBytes() == 0) {
                        return;
                    }
                    const QVariantMap first = tiles.front().toMap();
                    QSize decoded_size;
                    const QImage image = edit_preview_provider->requestImage(
                        image_provider_request_id(
                            first.value(QStringLiteral("source")).toString()
                        ),
                        &decoded_size,
                        {}
                    );
                    const QSize expected(
                        first.value(QStringLiteral("width")).toInt(),
                        first.value(QStringLiteral("height")).toInt()
                    );
                    if (image.isNull() || decoded_size != expected
                        || image.colorSpace() != QColorSpace(QColorSpace::SRgb)) {
                        return;
                    }
                    *succeeded = true;
                    QTimer::singleShot(50, &application, &QCoreApplication::quit);
                }
            );
            QTimer::singleShot(120'000, &application, [&application, succeeded]() {
                application.exit(*succeeded ? EXIT_SUCCESS : EXIT_FAILURE);
            });
        } else if (open_first_edit && adjustment_stack_smoke) {
            AdjustmentStackSmoke::start(application, controller, editor);
        } else if (open_first_edit) {
            QObject::connect(
                &editor,
                &EditController::previewSourceChanged,
                &application,
                [&application, &editor, request_before]() {
                    if (!editor.previewSource().isEmpty()) {
                        if (request_before) {
                            editor.requestBeforePreview();
                        } else {
                            QTimer::singleShot(50, &application, &QCoreApplication::quit);
                        }
                    }
                }
            );
            if (request_before) {
                QObject::connect(
                    &editor,
                    &EditController::beforePreviewSourceChanged,
                    &application,
                    [&application, &editor]() {
                        if (!editor.beforePreviewSource().isEmpty()) {
                            QTimer::singleShot(50, &application, &QCoreApplication::quit);
                        }
                    }
                );
            }
            QTimer::singleShot(
                30'000,
                &application,
                [&application, &editor, request_before]() {
                    const bool succeeded = !editor.previewSource().isEmpty()
                        && (!request_before || !editor.beforePreviewSource().isEmpty());
                    application.exit(succeeded ? EXIT_SUCCESS : EXIT_FAILURE);
                }
            );
        } else {
            QTimer::singleShot(500, &application, &QCoreApplication::quit);
        }
    }
    return application.exec();
}
