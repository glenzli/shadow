#include "desktop_backend.hpp"
#include "edit_controller.hpp"
#include "edit_preview_provider.hpp"
#include "lut_library.hpp"
#include "lut_preview_provider.hpp"
#include "review_controller.hpp"
#include "thumbnail_provider.hpp"
#include "ui_preferences.hpp"

#if defined(Q_OS_MACOS)
#include "mac_titlebar.hpp"
#endif

#include <QDebug>
#include <QApplication>
#include <QColorSpace>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QMessageBox>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QSize>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QVariant>
#include <QVector>
#include <QWindow>

#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>

namespace {

[[nodiscard]] bool reset_local_development_catalog(
    const QString& catalog_path,
    const QString& cache_root,
    QString* const error_message
) {
    const QStringList catalog_files{
        catalog_path,
        catalog_path + QStringLiteral("-wal"),
        catalog_path + QStringLiteral("-shm"),
    };
    for (const QString& path : catalog_files) {
        if (QFileInfo::exists(path) && !QFile::remove(path)) {
            *error_message = QObject::tr("Could not remove %1.").arg(path);
            return false;
        }
    }

    QDir cache_directory(cache_root);
    if (cache_directory.exists() && !cache_directory.removeRecursively()) {
        *error_message = QObject::tr("Could not remove the local preview cache.");
        return false;
    }
    return true;
}

[[nodiscard]] bool is_development_catalog_reset_error(const std::exception& error) {
    return QString::fromUtf8(error.what()).contains(
        QStringLiteral("development catalog reset required")
    );
}

[[nodiscard]] QMessageBox::StandardButton offer_development_catalog_reset(
    const std::exception& error
) {
    const QString detail = QString::fromUtf8(error.what());
    const bool incompatible = is_development_catalog_reset_error(error);
    const QString explanation = incompatible
        ? QObject::tr(
              "This local catalog belongs to an incompatible development build. "
              "Shadow does not migrate development schemas.\n\n"
              "Resetting removes the local photo index, edit history, and preview cache. "
              "Your original photo files, LUT library, and UI preferences are not changed."
          )
        : QObject::tr(
              "Shadow could not open its local development catalog. You can reset it "
              "and start again with a fresh catalog v1.\n\n"
              "Resetting removes the local photo index, edit history, and preview cache. "
              "Your original photo files, LUT library, and UI preferences are not changed.\n\n"
              "Technical detail: %1"
          ).arg(detail);
    return QMessageBox::warning(
        nullptr,
        QObject::tr("Reset local development catalog?"),
        explanation,
        QMessageBox::Reset | QMessageBox::Cancel,
        QMessageBox::Reset
    );
}

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

[[nodiscard]] QString preview_generation(const QString& source) {
    return QUrlQuery(QUrl(source)).queryItemValue(QStringLiteral("generation"));
}

[[nodiscard]] bool valid_edit_histogram(
    const QVariantMap& histogram,
    const QString& source
) {
    const QString source_generation = preview_generation(source);
    if (!histogram.value(QStringLiteral("valid")).toBool()
        || histogram.value(QStringLiteral("updating")).toBool()
        || histogram.value(QStringLiteral("stale")).toBool()
        || histogram.value(QStringLiteral("version")).toString().isEmpty()
        || source_generation.isEmpty()
        || histogram.value(QStringLiteral("generation")).toString() != source_generation
        || histogram.value(QStringLiteral("targetGeneration")).toString()
            != source_generation) {
        return false;
    }
    const qulonglong pixel_count = histogram.value(QStringLiteral("pixelCount")).toULongLong();
    const qulonglong dimensions_count =
        histogram.value(QStringLiteral("width")).toULongLong()
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
        && histogram.value(QStringLiteral("shadowClippedPixels")).toULongLong()
            <= pixel_count
        && histogram.value(QStringLiteral("highlightClippedPixels")).toULongLong()
            <= pixel_count;
}

[[nodiscard]] QObject* precision_workspace(QQmlApplicationEngine& engine) {
    if (engine.rootObjects().isEmpty()) {
        return nullptr;
    }
    return engine.rootObjects().front()->findChild<QObject*>(
        QStringLiteral("precisionWorkspace")
    );
}

[[nodiscard]] int review_grid_count(QQmlApplicationEngine& engine) {
    if (engine.rootObjects().isEmpty()) {
        return 0;
    }
    const auto* const grid = engine.rootObjects().front()->findChild<QObject*>(
        QStringLiteral("reviewGrid")
    );
    return grid == nullptr ? 0 : grid->property("count").toInt();
}

[[nodiscard]] bool qml_preview_is_ready(
    QQmlApplicationEngine& engine,
    const QString& source
) {
    const auto* const workspace = precision_workspace(engine);
    return workspace != nullptr && !preview_generation(source).isEmpty()
        && workspace->property("readyPreviewGeneration").toString()
            == preview_generation(source);
}

void after_qml_preview_ready(
    QCoreApplication& application,
    QQmlApplicationEngine& engine,
    QString source,
    std::function<void()> action
) {
    auto poll = std::make_shared<std::function<void()>>();
    *poll = [&application, &engine, source = std::move(source),
             action = std::move(action), poll]() {
        if (qml_preview_is_ready(engine, source)) {
            *poll = {};
            action();
            return;
        }
        QTimer::singleShot(20, &application, *poll);
    };
    QTimer::singleShot(0, &application, *poll);
}

class GradeStackSmoke final
    : public std::enable_shared_from_this<GradeStackSmoke> {
public:
    static void start(
        QCoreApplication& application,
        ReviewController& review,
        EditController& editor
    ) {
        const auto smoke = std::shared_ptr<GradeStackSmoke>(
            new GradeStackSmoke(application, review, editor)
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

    GradeStackSmoke(
        QCoreApplication& application,
        ReviewController& review,
        EditController& editor
    )
        : application_(application), review_(review), editor_(editor) {}

    static QString gradeNodeId(
        const QVariantList& grade_nodes,
        const qsizetype index
    ) {
        return grade_nodes.at(index)
            .toMap()
            .value(QStringLiteral("gradeNodeId"))
            .toString();
    }

    static bool gradeNodeEnabled(
        const QVariantList& grade_nodes,
        const qsizetype index
    ) {
        return grade_nodes.at(index)
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

        const QVariantList initial_grade_nodes = editor_.gradeNodes();
        if (!expect(
                initial_grade_nodes.size() == 1,
                QStringLiteral("expected one initial Grade Node, found %1")
                    .arg(initial_grade_nodes.size())
            )) {
            return;
        }
        const QString initial_id = gradeNodeId(initial_grade_nodes, 0);
        if (!expect(
                !initial_id.isEmpty(),
                QStringLiteral("initial Grade Node has no ID")
            )) {
            return;
        }

        editor_.addGradeNode();
        if (!expect(
                editor_.gradeNodes().size() == 2,
                QStringLiteral("add Grade Node failed")
            )) {
            return;
        }
        const QString added_id = editor_.selectedGradeNodeId();
        editor_.setExposureStops(expected_exposure_);
        editor_.duplicateSelectedGradeNode();
        if (!expect(
                editor_.gradeNodes().size() == 3,
                QStringLiteral("duplicate Grade Node failed")
            )) {
            return;
        }
        const QString duplicate_id = editor_.selectedGradeNodeId();
        if (!expect(
                !added_id.isEmpty() && !duplicate_id.isEmpty()
                    && added_id != initial_id && duplicate_id != initial_id
                    && duplicate_id != added_id,
                QStringLiteral("new Grade Nodes did not receive unique stable IDs")
            )) {
            return;
        }

        editor_.moveSelectedGradeNode(0);
        editor_.setGradeNodeEnabled(false);
        expected_grade_node_ids_ = {duplicate_id, initial_id, added_id};
        const QVariantList adjusted_grade_nodes = editor_.gradeNodes();
        if (!expect(
                adjusted_grade_nodes.size() == 3
                    && gradeNodeId(adjusted_grade_nodes, 0) == duplicate_id
                    && gradeNodeId(adjusted_grade_nodes, 1) == initial_id
                    && gradeNodeId(adjusted_grade_nodes, 2) == added_id
                    && !gradeNodeEnabled(adjusted_grade_nodes, 0),
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
        editor_.saveVersion(QStringLiteral("Grade Stack Smoke"));
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
        const QVariantList grade_nodes = editor_.gradeNodes();
        if (!expect(
                !editor_.stateBusy() && !editor_.rendering() && !editor_.dirty()
                    && grade_nodes.size() == expected_grade_node_ids_.size(),
                QStringLiteral("reopened stack was not clean and settled")
            )) {
            return;
        }
        for (qsizetype index = 0; index < expected_grade_node_ids_.size(); ++index) {
            if (!expect(
                    gradeNodeId(grade_nodes, index)
                        == expected_grade_node_ids_.at(index),
                    QStringLiteral("stable Grade Node order changed after reopen")
                )) {
                return;
            }
        }
        if (!expect(
                !gradeNodeEnabled(grade_nodes, 0)
                    && gradeNodeEnabled(grade_nodes, 2),
                QStringLiteral("bypass state changed after reopen")
            )) {
            return;
        }

        editor_.selectGradeNode(0);
        const bool duplicate_parameter_ok =
            editor_.selectedGradeNodeId() == expected_grade_node_ids_.at(0)
            && std::abs(editor_.exposureStops() - expected_exposure_) < 1.0e-9;
        editor_.selectGradeNode(2);
        const bool source_parameter_ok =
            editor_.selectedGradeNodeId() == expected_grade_node_ids_.at(2)
            && std::abs(editor_.exposureStops() - expected_exposure_) < 1.0e-9;
        if (!expect(
                duplicate_parameter_ok && source_parameter_ok,
                QStringLiteral("Grade Node parameters changed after reopen")
            )) {
            return;
        }

        stage_ = Stage::Finished;
        qInfo() << "Grade Stack smoke passed with three persisted Grade Nodes";
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
        qCritical().noquote() << "Grade Stack smoke failed:" << reason;
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
    QVector<QString> expected_grade_node_ids_;
    const double expected_exposure_ = 0.75;
};

} // namespace

int main(int argc, char* argv[]) {
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QApplication application(argc, argv);
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
    const QString isolated_settings_file =
        qEnvironmentVariableIsSet("SHADOW_DESKTOP_DATA_ROOT")
        ? QDir(application_data).filePath(QStringLiteral("ui-preferences.ini"))
        : QString{};
    UiPreferences preferences(application, isolated_settings_file);
    LutLibrary lut_library(
        isolated_settings_file,
        QDir(application_data).filePath(QStringLiteral("lut-store"))
    );

    std::shared_ptr<DesktopBackend> backend;
    while (!backend) {
        try {
            backend = std::make_shared<DesktopBackend>(catalog_path, cache_root);
        } catch (const std::exception& error) {
            qCritical() << "Cannot start Shadow's local backend:" << error.what();
            const auto choice = offer_development_catalog_reset(error);
            if (choice != QMessageBox::Reset) {
                return EXIT_FAILURE;
            }

            QString reset_error;
            if (!reset_local_development_catalog(catalog_path, cache_root, &reset_error)) {
                QMessageBox::critical(
                    nullptr,
                    QObject::tr("Catalog reset failed"),
                    reset_error
                );
                return EXIT_FAILURE;
            }
        }
    }
    ReviewController controller(backend, isolated_settings_file);
    auto edit_preview_store = std::make_shared<EditPreviewStore>();
    EditController editor(backend, edit_preview_store);
    QQmlApplicationEngine engine;
    preferences.attachEngine(engine);
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
    engine.addImageProvider(
        QStringLiteral("shadow-lut"),
        new LutPreviewProvider(
            QDir(application_data).filePath(QStringLiteral("lut-store")),
            QDir(cache_root).filePath(QStringLiteral("lut-previews"))
        )
    );
    engine.setInitialProperties({
        {QStringLiteral("controller"), QVariant::fromValue(&controller)},
        {QStringLiteral("editor"), QVariant::fromValue(&editor)},
        {QStringLiteral("preferences"), QVariant::fromValue(&preferences)},
        {QStringLiteral("lutLibrary"), QVariant::fromValue(&lut_library)},
    });
    engine.loadFromModule("Shadow.App", "Main");
    if (engine.rootObjects().isEmpty()) {
        return EXIT_FAILURE;
    }
    if (qEnvironmentVariableIsSet("SHADOW_DESKTOP_OPEN_LUT_LIBRARY")) {
        QMetaObject::invokeMethod(
            engine.rootObjects().front(),
            "openLutManager",
            Qt::DirectConnection
        );
    }
#if defined(Q_OS_MACOS)
    QObject* const root_object = engine.rootObjects().constFirst();
    QObject* const title_toolbar = root_object->findChild<QObject*>(
        QStringLiteral("titleToolBar")
    );
    const int title_bar_height = title_toolbar == nullptr
        ? 44
        : qRound(title_toolbar->property("height").toReal());
    installMacTitleBarAlignment(
        qobject_cast<QWindow*>(root_object),
        title_bar_height
    );
#endif
    QString initial_folder = qEnvironmentVariable("SHADOW_DESKTOP_SCAN_FOLDER");
#if defined(SHADOW_DESKTOP_DEV_SAMPLE_FOLDER)
    if (initial_folder.isEmpty()) {
        const QDir development_samples(
            QString::fromUtf8(SHADOW_DESKTOP_DEV_SAMPLE_FOLDER)
        );
        if (development_samples.exists()) {
            initial_folder = development_samples.absolutePath();
        }
    }
#endif
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
    const bool grade_stack_smoke = qEnvironmentVariableIsSet(
        "SHADOW_DESKTOP_GRADE_STACK_SMOKE"
    );
    const bool full_detail_smoke = qEnvironmentVariableIsSet(
        "SHADOW_DESKTOP_FULL_DETAIL_SMOKE"
    );
    const bool streaming_scan_smoke = qEnvironmentVariableIsSet(
        "SHADOW_DESKTOP_STREAMING_SCAN_SMOKE"
    );
    const bool reopen_library_smoke = qEnvironmentVariableIsSet(
        "SHADOW_DESKTOP_REOPEN_LIBRARY_SMOKE"
    );
    const bool cancel_scan_smoke = qEnvironmentVariableIsSet(
        "SHADOW_DESKTOP_CANCEL_SCAN_SMOKE"
    );
    const bool i18n_smoke = qEnvironmentVariableIsSet(
        "SHADOW_DESKTOP_I18N_SMOKE"
    );
    const bool close_lifecycle_smoke = qEnvironmentVariableIsSet(
        "SHADOW_DESKTOP_CLOSE_SMOKE"
    );
    const bool dirty_close_smoke = qEnvironmentVariableIsSet(
        "SHADOW_DESKTOP_DIRTY_CLOSE_SMOKE"
    );
    if (open_first_edit && !record_first_comparison && !set_first_decision) {
        QObject::connect(
            &controller,
            &ReviewController::itemCountChanged,
            &application,
            [&controller, &editor, &engine]() {
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
                if (!engine.rootObjects().isEmpty()) {
                    engine.rootObjects().front()->setProperty("workspaceIndex", 1);
                }
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
                    || controller.scanning() || controller.refreshing()
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
        if (dirty_close_smoke) {
            auto close_started = std::make_shared<bool>(false);
            auto attempt_close = std::make_shared<std::function<void()>>();
            *attempt_close = [
                &application,
                &editor,
                &engine,
                close_started
            ]() {
                if (*close_started || !editor.active() || editor.stateBusy()
                    || editor.gradeNodes().isEmpty()) {
                    return;
                }
                *close_started = true;
                editor.setExposureStops(editor.exposureStops() + 0.25);
                if (!editor.dirty()) {
                    qCritical() << "Dirty close smoke could not create working changes";
                    application.exit(EXIT_FAILURE);
                    return;
                }
                auto* const window = qobject_cast<QWindow*>(
                    engine.rootObjects().front()
                );
                if (window == nullptr || window->close()) {
                    qCritical() << "Dirty close smoke did not defer for autosave";
                    application.exit(EXIT_FAILURE);
                    return;
                }
                QTimer::singleShot(
                    0,
                    &application,
                    [&application, &engine]() {
                        QObject* const root_object = engine.rootObjects().front();
                        QObject* const dialog = root_object->findChild<QObject*>(
                            QStringLiteral("discardQuitDialog")
                        );
                        if (dialog != nullptr) {
                            qCritical() << "Dirty close smoke found an obsolete discard dialog";
                            application.exit(EXIT_FAILURE);
                        }
                    }
                );
            };
            QObject::connect(
                &editor,
                &EditController::stateBusyChanged,
                &application,
                [attempt_close]() { (*attempt_close)(); }
            );
            QObject::connect(
                &editor,
                &EditController::gradeNodesChanged,
                &application,
                [attempt_close]() { (*attempt_close)(); }
            );
            QTimer::singleShot(30'000, &application, [&application]() {
                qCritical() << "Dirty close lifecycle smoke timed out";
                application.exit(EXIT_FAILURE);
            });
        } else if (close_lifecycle_smoke) {
            auto* const window = qobject_cast<QWindow*>(engine.rootObjects().front());
            if (window == nullptr) {
                qCritical() << "Close lifecycle smoke could not find the main window";
                application.exit(EXIT_FAILURE);
            } else {
                QTimer::singleShot(0, window, [window]() { window->close(); });
            }
            QTimer::singleShot(10'000, &application, [&application]() {
                qCritical() << "Close lifecycle smoke timed out";
                application.exit(EXIT_FAILURE);
            });
        } else if (i18n_smoke) {
            QObject* const root_object = engine.rootObjects().front();
            QObject* const settings_button = root_object->findChild<QObject*>(
                QStringLiteral("settingsButton")
            );
            if (settings_button == nullptr) {
                qCritical() << "I18n smoke could not find the Settings control";
                application.exit(EXIT_FAILURE);
            } else {
                preferences.setLanguageMode(QStringLiteral("en"));
                preferences.setLanguageMode(QStringLiteral("zh_CN"));
                QTimer::singleShot(
                    0,
                    &application,
                    [&application, &controller, &preferences, root_object,
                     settings_button]() {
                        const QString chinese_controller_status =
                            controller.statusText();
                        if (preferences.effectiveLanguage()
                                != QStringLiteral("zh_CN")
                            || settings_button->property("text").toString()
                                != QString::fromUtf8("设置")
                            || chinese_controller_status.isEmpty()) {
                            qCritical() << "I18n smoke did not apply Simplified Chinese";
                            application.exit(EXIT_FAILURE);
                            return;
                        }
                        preferences.setLanguageMode(QStringLiteral("en"));
                        QTimer::singleShot(
                            0,
                            &application,
                            [&application, &controller, &preferences, root_object,
                             settings_button, chinese_controller_status]() {
                                QObject* const current_button =
                                    root_object->findChild<QObject*>(
                                        QStringLiteral("settingsButton")
                                    );
                                if (current_button != settings_button
                                    || preferences.effectiveLanguage()
                                        != QStringLiteral("en")
                                    || settings_button->property("text").toString()
                                        != QStringLiteral("Settings")
                                    || controller.statusText()
                                        == chinese_controller_status) {
                                    qCritical() << "I18n smoke did not retranslate in place";
                                    application.exit(EXIT_FAILURE);
                                    return;
                                }
                                const QString english_controller_status =
                                    controller.statusText();
                                preferences.setLanguageMode(QStringLiteral("zh_CN"));
                                QTimer::singleShot(
                                    0,
                                    &application,
                                    [&application, &controller, &preferences,
                                     settings_button, english_controller_status]() {
                                        const bool succeeded =
                                            preferences.effectiveLanguage()
                                                == QStringLiteral("zh_CN")
                                            && settings_button
                                                   ->property("text")
                                                   .toString()
                                                == QString::fromUtf8("设置")
                                            && !controller.statusText().isEmpty()
                                            && controller.statusText()
                                                != english_controller_status;
                                        if (!succeeded) {
                                            qCritical()
                                                << "I18n smoke did not restore Simplified Chinese"
                                                << "language"
                                                << preferences.effectiveLanguage()
                                                << "button"
                                                << settings_button
                                                       ->property("text")
                                                       .toString()
                                                << "status"
                                                << controller.statusText()
                                                << "English status"
                                                << english_controller_status;
                                        }
                                        application.exit(
                                            succeeded ? EXIT_SUCCESS : EXIT_FAILURE
                                        );
                                    }
                                );
                            }
                        );
                    }
                );
            }
            QTimer::singleShot(10'000, &application, [&application]() {
                application.exit(EXIT_FAILURE);
            });
        } else if (cancel_scan_smoke) {
            auto cancel_requested = std::make_shared<bool>(false);
            auto succeeded = std::make_shared<bool>(false);
            QObject::connect(
                &controller,
                &ReviewController::scanProgressChanged,
                &application,
                [&controller, cancel_requested]() {
                    if (*cancel_requested || !controller.scanning()
                        || controller.scanProgress()
                               .value(QStringLiteral("filesSeen"))
                               .toULongLong()
                            == 0) {
                        return;
                    }
                    *cancel_requested = true;
                    controller.cancelScan();
                }
            );
            const auto finish_cancel_smoke = [
                &application,
                &controller,
                cancel_requested,
                succeeded
            ]() {
                if (*succeeded || !*cancel_requested || controller.scanning()
                    || controller.refreshing()
                    || controller.statusText().startsWith(
                        QStringLiteral("Final Library refresh failed")
                    )
                    || controller.scanProgress()
                           .value(QStringLiteral("phase"))
                           .toString()
                        != QStringLiteral("cancelled")) {
                    return;
                }
                *succeeded = true;
                qInfo() << "Cooperative import cancellation smoke passed";
                QTimer::singleShot(50, &application, &QCoreApplication::quit);
            };
            QObject::connect(
                &controller,
                &ReviewController::itemCountChanged,
                &application,
                finish_cancel_smoke
            );
            QObject::connect(
                &controller,
                &ReviewController::refreshingChanged,
                &application,
                finish_cancel_smoke
            );
            QTimer::singleShot(30'000, &application, [
                &application,
                cancel_requested,
                succeeded
            ]() {
                if (!*succeeded) {
                    qCritical() << "Cooperative import cancellation smoke failed"
                                << "requested" << *cancel_requested;
                    application.exit(EXIT_FAILURE);
                }
            });
        } else if (streaming_scan_smoke) {
            auto early_model_visible = std::make_shared<bool>(false);
            auto early_qml_visible = std::make_shared<bool>(false);
            auto succeeded = std::make_shared<bool>(false);
            auto evaluate = std::make_shared<std::function<void()>>();
            *evaluate = [
                &application,
                &controller,
                &engine,
                early_model_visible,
                early_qml_visible,
                succeeded
            ]() {
                if (*succeeded || !*early_model_visible || !*early_qml_visible
                    || controller.scanning() || controller.refreshing()
                    || controller.reviewModel()->rowCount() == 0
                    || review_grid_count(engine) == 0
                    || controller.statusText().startsWith(
                        QStringLiteral("Final Library refresh failed")
                    )
                    || controller.scanProgress()
                           .value(QStringLiteral("phase"))
                           .toString()
                        != QStringLiteral("completed")) {
                    return;
                }
                *succeeded = true;
                qInfo() << "Streaming import smoke displayed the first page before completion";
                QTimer::singleShot(50, &application, &QCoreApplication::quit);
            };
            QObject::connect(
                &controller,
                &ReviewController::itemCountChanged,
                &application,
                [
                    &application,
                    &controller,
                    &engine,
                    early_model_visible,
                    early_qml_visible,
                    evaluate
                ]() {
                    if (controller.scanning()
                        && controller.reviewModel()->rowCount() > 0) {
                        *early_model_visible = true;
                        QTimer::singleShot(
                            0,
                            &application,
                            [&controller, &engine, early_qml_visible, evaluate]() {
                                if (controller.scanning()
                                    && controller.reviewModel()->rowCount() > 0
                                    && review_grid_count(engine) > 0) {
                                    *early_qml_visible = true;
                                }
                                (*evaluate)();
                            }
                        );
                    }
                    (*evaluate)();
                }
            );
            QObject::connect(
                &controller,
                &ReviewController::scanningChanged,
                &application,
                [evaluate]() { (*evaluate)(); }
            );
            QObject::connect(
                &controller,
                &ReviewController::refreshingChanged,
                &application,
                [evaluate]() { (*evaluate)(); }
            );
            QTimer::singleShot(120'000, &application, [
                &application,
                early_model_visible,
                early_qml_visible,
                succeeded
            ]() {
                if (!*succeeded) {
                    qCritical() << "Streaming import smoke failed"
                                << "early model" << *early_model_visible
                                << "early QML" << *early_qml_visible;
                    application.exit(EXIT_FAILURE);
                }
            });
        } else if (reopen_library_smoke) {
            auto succeeded = std::make_shared<bool>(false);
            QObject::connect(
                &controller,
                &ReviewController::itemCountChanged,
                &application,
                [&application, &controller, &engine, succeeded]() {
                    QTimer::singleShot(
                        0,
                        &application,
                        [&application, &controller, &engine, succeeded]() {
                            if (*succeeded || controller.scanning()
                                || controller.refreshing()
                                || controller.reviewModel()->rowCount() == 0
                                || review_grid_count(engine) == 0) {
                                return;
                            }
                            *succeeded = true;
                            qInfo() << "Reopen smoke loaded the persisted Library without scanning";
                            QTimer::singleShot(
                                50,
                                &application,
                                &QCoreApplication::quit
                            );
                        }
                    );
                }
            );
            QTimer::singleShot(30'000, &application, [&application, succeeded]() {
                application.exit(*succeeded ? EXIT_SUCCESS : EXIT_FAILURE);
            });
        } else if (record_first_comparison) {
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
                [&application, &engine, &editor, requested]() {
                    const QString source = editor.previewSource();
                    if (*requested || source.isEmpty()
                        || !valid_edit_histogram(editor.histogram(), source)) {
                        return;
                    }
                    *requested = true;
                    after_qml_preview_ready(
                        application,
                        engine,
                        source,
                        [&editor]() {
                            editor.requestDetailViewport(0.5, 0.5, 1'280, 960);
                        }
                    );
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
        } else if (open_first_edit && grade_stack_smoke) {
            GradeStackSmoke::start(application, controller, editor);
        } else if (open_first_edit) {
            auto current_wait_started = std::make_shared<bool>(false);
            auto before_wait_started = std::make_shared<bool>(false);
            QObject::connect(
                &editor,
                &EditController::previewSourceChanged,
                &application,
                [&application, &engine, &editor, request_before,
                 current_wait_started]() {
                    const QString source = editor.previewSource();
                    if (*current_wait_started || source.isEmpty()
                        || !valid_edit_histogram(editor.histogram(), source)) {
                        return;
                    }
                    *current_wait_started = true;
                    after_qml_preview_ready(
                        application,
                        engine,
                        source,
                        [&application, &engine, &editor, request_before]() {
                            if (request_before) {
                                if (auto* const workspace = precision_workspace(engine)) {
                                    workspace->setProperty("showBefore", true);
                                }
                                editor.requestBeforePreview();
                            } else {
                                QTimer::singleShot(
                                    50,
                                    &application,
                                    &QCoreApplication::quit
                                );
                            }
                        }
                    );
                }
            );
            if (request_before) {
                QObject::connect(
                    &editor,
                    &EditController::beforePreviewSourceChanged,
                    &application,
                    [&application, &engine, &editor, before_wait_started]() {
                        const QString source = editor.beforePreviewSource();
                        if (*before_wait_started || source.isEmpty()
                            || !valid_edit_histogram(
                                editor.beforeHistogram(),
                                source
                            )) {
                            return;
                        }
                        *before_wait_started = true;
                        after_qml_preview_ready(
                            application,
                            engine,
                            source,
                            [&application]() {
                                QTimer::singleShot(
                                    50,
                                    &application,
                                    &QCoreApplication::quit
                                );
                            }
                        );
                    }
                );
            }
            QTimer::singleShot(
                30'000,
                &application,
                [&application, &engine, &editor, request_before]() {
                    const QString current_source = editor.previewSource();
                    const QString before_source = editor.beforePreviewSource();
                    const bool succeeded = !current_source.isEmpty()
                        && valid_edit_histogram(editor.histogram(), current_source)
                        && qml_preview_is_ready(engine, current_source)
                        && (!request_before
                            || (!before_source.isEmpty()
                                && valid_edit_histogram(
                                    editor.beforeHistogram(),
                                    before_source
                                )
                                && qml_preview_is_ready(engine, before_source)));
                    application.exit(succeeded ? EXIT_SUCCESS : EXIT_FAILURE);
                }
            );
        } else {
            QTimer::singleShot(500, &application, &QCoreApplication::quit);
        }
    }
    return application.exec();
}
