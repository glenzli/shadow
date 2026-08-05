#include <QCoreApplication>
#include <QMetaObject>
#include <QObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QString>
#include <QUrl>
#include <QVariantMap>

#include <cstdlib>
#include <iostream>
#include <memory>

class FakeReviewSelection final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString selectedPhotoId MEMBER photo_id NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedRepresentationId MEMBER representation_id NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedVisualHandle MEMBER visual_handle NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedTitle MEMBER title NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedPath MEMBER source_path NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedRole MEMBER visual_role NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedVisualSource MEMBER visual_source NOTIFY selectionChanged)
    Q_PROPERTY(int selectedWidth MEMBER visual_width NOTIFY selectionChanged)
    Q_PROPERTY(int selectedHeight MEMBER visual_height NOTIFY selectionChanged)
    Q_PROPERTY(
        bool selectedHasTechnicalObservation MEMBER has_technical_observation NOTIFY
            selectionChanged
    )
    Q_PROPERTY(int selectedTechnicalInputWidth MEMBER technical_input_width NOTIFY selectionChanged)
    Q_PROPERTY(
        int selectedTechnicalInputHeight MEMBER technical_input_height NOTIFY selectionChanged
    )
    Q_PROPERTY(
        QString selectedTechnicalPreprocessingVersion MEMBER technical_preprocessing_version NOTIFY
            selectionChanged
    )
    Q_PROPERTY(
        QString selectedTechnicalImplementationVersion MEMBER technical_implementation_version
            NOTIFY selectionChanged
    )
    Q_PROPERTY(double selectedMeanLuma MEMBER mean_luma NOTIFY selectionChanged)
    Q_PROPERTY(double selectedP01Luma MEMBER p01_luma NOTIFY selectionChanged)
    Q_PROPERTY(double selectedP50Luma MEMBER p50_luma NOTIFY selectionChanged)
    Q_PROPERTY(double selectedP99Luma MEMBER p99_luma NOTIFY selectionChanged)
    Q_PROPERTY(double selectedNearBlackFraction MEMBER near_black_fraction NOTIFY selectionChanged)
    Q_PROPERTY(double selectedNearWhiteFraction MEMBER near_white_fraction NOTIFY selectionChanged)
    Q_PROPERTY(double selectedLaplacianVariance MEMBER laplacian_variance NOTIFY selectionChanged)
    Q_PROPERTY(double selectedEdgeEnergy MEMBER edge_energy NOTIFY selectionChanged)

  public:
    using QObject::QObject;

    void select(
        const QString& photo_id_value,
        const QString& representation_id_value,
        const QString& visual_handle_value,
        const QString& visual_source_value
    ) {
        photo_id = photo_id_value;
        representation_id = representation_id_value;
        visual_handle = visual_handle_value;
        visual_source = visual_source_value;
        title = photo_id_value;
        source_path = QStringLiteral("/photos/") + photo_id_value;
        emit selectionChanged();
    }

    Q_INVOKABLE void selectPhoto(const QVariantMap& target, int modifiers) {
        Q_UNUSED(modifiers)
        select(
            target.value(QStringLiteral("photoId")).toString(),
            target.value(QStringLiteral("representationId")).toString(),
            target.value(QStringLiteral("visualHandle")).toString(),
            target.value(QStringLiteral("visualSource")).toString()
        );
    }

    QString photo_id;
    QString representation_id;
    QString visual_handle;
    QString title;
    QString source_path;
    QString visual_role = QStringLiteral("generated_proxy");
    QString visual_source;
    int visual_width = 1'600;
    int visual_height = 1'200;
    bool has_technical_observation = true;
    int technical_input_width = 6'048;
    int technical_input_height = 4'024;
    QString technical_preprocessing_version = QStringLiteral("pre-v1");
    QString technical_implementation_version = QStringLiteral("impl-v1");
    double mean_luma = 0.42;
    double p01_luma = 0.01;
    double p50_luma = 0.40;
    double p99_luma = 0.98;
    double near_black_fraction = 0.02;
    double near_white_fraction = 0.03;
    double laplacian_variance = 12.0;
    double edge_energy = 4.0;

  signals:
    void selectionChanged();
};

class FakeComparisonController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool scanning MEMBER scanning NOTIFY stateChanged)
    Q_PROPERTY(bool refreshing MEMBER refreshing NOTIFY stateChanged)
    Q_PROPERTY(bool comparisonBusy MEMBER comparison_busy NOTIFY stateChanged)
    Q_PROPERTY(bool decisionBusy MEMBER decision_busy NOTIFY stateChanged)
    Q_PROPERTY(int itemCount MEMBER item_count NOTIFY itemCountChanged)

  public:
    using QObject::QObject;

    Q_INVOKABLE QVariantMap
    prepareComparison(const QString& left_handle, const QString& right_handle) {
        prepared_left_handle = left_handle;
        prepared_right_handle = right_handle;
        return {
            {QStringLiteral("presentationId"), QStringLiteral("presentation-1")},
            {QStringLiteral("leftRequestTicket"), QStringLiteral("left-ticket")},
            {QStringLiteral("rightRequestTicket"), QStringLiteral("right-ticket")},
            {QStringLiteral("leftSource"), QStringLiteral("image://left")},
            {QStringLiteral("rightSource"), QStringLiteral("image://right")},
        };
    }

    Q_INVOKABLE QVariantMap navigationTarget(
        const QString& photo_id,
        const QString& representation_id,
        const int horizontal_delta,
        const int vertical_delta
    ) const {
        Q_UNUSED(representation_id)
        Q_UNUSED(vertical_delta)
        QString target;
        if (horizontal_delta > 0 && photo_id == QStringLiteral("photo-a")) {
            target = QStringLiteral("photo-b");
        } else if (horizontal_delta > 0 && photo_id == QStringLiteral("photo-b")) {
            target = QStringLiteral("photo-c");
        } else if (horizontal_delta < 0 && photo_id == QStringLiteral("photo-c")) {
            target = QStringLiteral("photo-b");
        } else if (horizontal_delta < 0 && photo_id == QStringLiteral("photo-b")) {
            target = QStringLiteral("photo-a");
        }
        if (target.isEmpty()) {
            return {};
        }
        return {
            {QStringLiteral("photoId"), target},
            {QStringLiteral("representationId"), target + QStringLiteral("-representation")},
            {QStringLiteral("visualHandle"), target + QStringLiteral("-visual")},
            {QStringLiteral("visualSource"), QStringLiteral("image://") + target},
            {QStringLiteral("sourcePath"), QStringLiteral("/photos/") + target},
            {QStringLiteral("title"), target},
        };
    }

    Q_INVOKABLE void cancelComparison(const QString& presentation_id) {
        cancelled_presentation_id = presentation_id;
    }

    Q_INVOKABLE bool confirmComparisonReady(
        const QString& presentation_id,
        const QString& left_ticket,
        const QString& right_ticket
    ) {
        confirmed_presentation_id = presentation_id;
        confirmed_left_ticket = left_ticket;
        confirmed_right_ticket = right_ticket;
        return true;
    }

    Q_INVOKABLE void recordComparison(const QString& presentation_id, const int outcome) {
        recorded_presentation_id = presentation_id;
        recorded_outcome = outcome;
    }

    bool scanning = false;
    bool refreshing = false;
    bool comparison_busy = false;
    bool decision_busy = false;
    int item_count = 2;
    QString prepared_left_handle;
    QString prepared_right_handle;
    QString cancelled_presentation_id;
    QString confirmed_presentation_id;
    QString confirmed_left_ticket;
    QString confirmed_right_ticket;
    QString recorded_presentation_id;
    int recorded_outcome = -1;

  signals:
    void stateChanged();
    void itemCountChanged();
    void comparisonRecorded();
    void comparisonForgotten();
};

namespace {

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "review comparison state contract failed: " << message << '\n';
    }
    return condition;
}

[[nodiscard]] bool invoke(QObject* target, const char* method) {
    return QMetaObject::invokeMethod(target, method);
}

[[nodiscard]] bool invoke(QObject* target, const char* method, const QVariant& argument) {
    return QMetaObject::invokeMethod(target, method, Q_ARG(QVariant, argument));
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    QQmlEngine engine;
    QQmlComponent component(
        &engine,
        QUrl::fromLocalFile(
            QStringLiteral(SHADOW_DESKTOP_SOURCE_DIR "/qml/ReviewComparisonState.qml")
        )
    );
    FakeReviewSelection selection;
    FakeComparisonController controller;
    selection.select(
        QStringLiteral("photo-a"),
        QStringLiteral("representation-a"),
        QStringLiteral("visual-a"),
        QStringLiteral("image://photo-a")
    );
    std::unique_ptr<QObject> comparison(component.createWithInitialProperties({
        {QStringLiteral("controller"), QVariant::fromValue(&controller)},
        {QStringLiteral("selection"), QVariant::fromValue(&selection)},
        {QStringLiteral("navigationModel"), QVariant::fromValue(&controller)},
    }));
    if (!comparison) {
        std::cerr << component.errorString().toStdString();
        return EXIT_FAILURE;
    }

    if (!require(invoke(comparison.get(), "setSelectedAsLeft"), "left-slot action is invokable")
        || !require(
            comparison->property("leftComparisonSnapshot")
                    .toMap()
                    .value(QStringLiteral("photoId"))
                    .toString()
                == QStringLiteral("photo-a"),
            "the slot owns a value snapshot of the selected identity"
        )
        || !require(
            invoke(comparison.get(), "setSelectedAsRight"),
            "right-slot action is invokable"
        )
        || !require(
            comparison->property("rightComparisonSnapshot").isNull()
                && comparison->property("localComparisonStatusKey").toString()
                       == QStringLiteral("photo-in-both-slots"),
            "one photo cannot occupy both evidence slots"
        )) {
        return EXIT_FAILURE;
    }

    selection.select(
        QStringLiteral("photo-b"),
        QStringLiteral("representation-b"),
        QStringLiteral("visual-b"),
        QStringLiteral("image://photo-b")
    );
    if (!require(
            invoke(comparison.get(), "setSelectedAsRight")
                && comparison->property("comparisonReady").toBool(),
            "two distinct snapshots make the comparison ready"
        )
        || !require(
            invoke(comparison.get(), "enterComparison"),
            "prepared comparison action is invokable"
        )
        || !require(
            comparison->property("compareMode").toBool()
                && controller.prepared_left_handle == QStringLiteral("visual-a")
                && controller.prepared_right_handle == QStringLiteral("visual-b"),
            "entering comparison prepares the exact frozen visual handles"
        )) {
        return EXIT_FAILURE;
    }

    comparison->setProperty("leftComparisonVisualReady", true);
    comparison->setProperty("rightComparisonVisualReady", true);
    if (!require(
            invoke(comparison.get(), "refreshComparisonReadiness"),
            "readiness refresh is invokable"
        )
        || !require(
            comparison->property("canSubmitComparison").toBool()
                && controller.confirmed_presentation_id == QStringLiteral("presentation-1")
                && controller.confirmed_left_ticket == QStringLiteral("left-ticket")
                && controller.confirmed_right_ticket == QStringLiteral("right-ticket"),
            "submission opens only after both visuals and backend receipts agree"
        )
        || !require(
            invoke(comparison.get(), "submitComparison", 3)
                && controller.recorded_presentation_id == QStringLiteral("presentation-1")
                && controller.recorded_outcome == 3,
            "the ready presentation and requested outcome are recorded together"
        )) {
        return EXIT_FAILURE;
    }

    emit controller.comparisonRecorded();
    QCoreApplication::processEvents();
    if (!require(
            !comparison->property("compareMode").toBool()
                && comparison->property("leftComparisonSnapshot").isNull()
                && comparison->property("rightComparisonSnapshot").isNull(),
            "record completion releases prepared state and both evidence slots"
        )) {
        return EXIT_FAILURE;
    }

    selection.select(
        QStringLiteral("photo-a"),
        QStringLiteral("photo-a-representation"),
        QStringLiteral("photo-a-visual"),
        QStringLiteral("image://photo-a")
    );
    if (!require(
            invoke(comparison.get(), "startSelectionComparison")
                && comparison->property("selectionCompareMode").toBool()
                && comparison->property("compareMode").toBool(),
            "quick comparison locks the selection and enters with an adjacent candidate"
        )
        || !require(
            comparison->property("leftComparisonSnapshot")
                        .toMap()
                        .value(QStringLiteral("photoId"))
                        .toString()
                    == QStringLiteral("photo-a")
                && comparison->property("rightComparisonSnapshot")
                           .toMap()
                           .value(QStringLiteral("photoId"))
                           .toString()
                       == QStringLiteral("photo-b"),
            "quick comparison preserves anchor and candidate identities"
        )) {
        return EXIT_FAILURE;
    }
    comparison->setProperty("leftComparisonVisualReady", true);
    comparison->setProperty("rightComparisonVisualReady", true);
    (void)invoke(comparison.get(), "refreshComparisonReadiness");
    if (!require(
            invoke(comparison.get(), "promoteCandidate") && controller.recorded_outcome == 1,
            "candidate promotion records an explicit right-preferred event"
        )) {
        return EXIT_FAILURE;
    }
    emit controller.comparisonRecorded();
    QCoreApplication::processEvents();
    if (!require(
            comparison->property("leftComparisonSnapshot")
                        .toMap()
                        .value(QStringLiteral("photoId"))
                        .toString()
                    == QStringLiteral("photo-b")
                && comparison->property("rightComparisonSnapshot")
                           .toMap()
                           .value(QStringLiteral("photoId"))
                           .toString()
                       == QStringLiteral("photo-c"),
            "successful promotion advances the locked anchor and next candidate"
        )) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

#include "review_comparison_state_contract_test.moc"
