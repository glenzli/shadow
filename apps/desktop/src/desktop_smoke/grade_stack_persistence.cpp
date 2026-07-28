#include "desktop_smoke/grade_stack_persistence.hpp"

#include "edit_controller.hpp"

#include <QCoreApplication>
#include <QDebug>
#include <QString>
#include <QTimer>
#include <QVariant>
#include <QVariantList>
#include <QVector>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <memory>

namespace {

class GradeStackPersistence final
    : public std::enable_shared_from_this<GradeStackPersistence> {
public:
    static void start(
        QCoreApplication& application,
        EditController& editor
    ) {
        const auto smoke = std::shared_ptr<GradeStackPersistence>(
            new GradeStackPersistence(application, editor)
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

    GradeStackPersistence(
        QCoreApplication& application,
        EditController& editor
    )
        : application_(application), editor_(editor) {}

    [[nodiscard]] static QString gradeNodeId(
        const QVariantList& grade_nodes,
        const qsizetype index
    ) {
        return grade_nodes.at(index)
            .toMap()
            .value(QStringLiteral("gradeNodeId"))
            .toString();
    }

    [[nodiscard]] static bool gradeNodeEnabled(
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
        QTimer::singleShot(0, &application_, [self]() {
            self->previewChanged();
        });
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
                    self->fail(
                        QStringLiteral("the saved photo could not be reloaded")
                    );
                }
            });
        }
    }

    void buildStack() {
        photo_id_ = editor_.photoId();
        representation_id_ = editor_.representationId();
        source_path_ = editor_.sourcePath();
        title_ = editor_.title();
        if (!expect(
                !photo_id_.isEmpty() && !representation_id_.isEmpty()
                    && !source_path_.isEmpty(),
                QStringLiteral("active photo identity is incomplete")
            )) {
            return;
        }

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
                QStringLiteral(
                    "new Grade Nodes did not receive unique stable IDs"
                )
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
        if (!expect(
                !editor_.active(),
                QStringLiteral("saved photo could not close")
            )) {
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

void DesktopSmoke::startGradeStackPersistence(
    QCoreApplication& application,
    EditController& editor
) {
    GradeStackPersistence::start(application, editor);
}
