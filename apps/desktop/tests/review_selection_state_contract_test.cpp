#include <QCoreApplication>
#include <QFileInfo>
#include <QMetaObject>
#include <QObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QString>
#include <QUrl>
#include <QVariant>
#include <QVariantMap>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <utility>

class FakeInspectionController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(
        QVariantMap photoInspection
        READ photoInspection
        NOTIFY photoInspectionChanged
    )
    Q_PROPERTY(
        bool photoInspectionBusy
        READ photoInspectionBusy
        NOTIFY photoInspectionChanged
    )
    Q_PROPERTY(
        bool photoInspectionFailed
        READ photoInspectionFailed
        NOTIFY photoInspectionChanged
    )

public:
    using QObject::QObject;

    [[nodiscard]] QVariantMap photoInspection() const {
        return inspection_;
    }
    [[nodiscard]] bool photoInspectionBusy() const noexcept {
        return false;
    }
    [[nodiscard]] bool photoInspectionFailed() const noexcept {
        return false;
    }

    Q_INVOKABLE void requestPhotoInspection(
        const QString& photo_id,
        const QString& representation_id
    ) {
        requested_photo_id_ = photo_id;
        requested_representation_id_ = representation_id;
    }

    Q_INVOKABLE void clearPhotoInspection() {
        ++clear_count_;
        inspection_.clear();
        emit photoInspectionChanged();
    }

    Q_INVOKABLE void retryPhotoInspection() {
        ++retry_count_;
    }

    void publish(QVariantMap inspection) {
        inspection_ = std::move(inspection);
        emit photoInspectionChanged();
    }

    QString requested_photo_id_;
    QString requested_representation_id_;
    int clear_count_ = 0;
    int retry_count_ = 0;

signals:
    void photoInspectionChanged();

private:
    QVariantMap inspection_;
};

class FakeReviewCard final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString photoId MEMBER photo_id CONSTANT)
    Q_PROPERTY(QString representationId MEMBER representation_id CONSTANT)
    Q_PROPERTY(QString visualHandle MEMBER visual_handle CONSTANT)
    Q_PROPERTY(qulonglong decisionHeadSequence MEMBER decision_head_sequence CONSTANT)
    Q_PROPERTY(QString decisionFlag MEMBER decision_flag CONSTANT)
    Q_PROPERTY(int decisionRating MEMBER decision_rating CONSTANT)
    Q_PROPERTY(bool liked MEMBER liked CONSTANT)
    Q_PROPERTY(QString colorLabel MEMBER color_label CONSTANT)
    Q_PROPERTY(QString title MEMBER title CONSTANT)
    Q_PROPERTY(QString sourcePath MEMBER source_path CONSTANT)
    Q_PROPERTY(bool sourceAvailable MEMBER source_available CONSTANT)
    Q_PROPERTY(QString visualRole MEMBER visual_role CONSTANT)
    Q_PROPERTY(QString visualSource MEMBER visual_source CONSTANT)
    Q_PROPERTY(int visualWidth MEMBER visual_width CONSTANT)
    Q_PROPERTY(int visualHeight MEMBER visual_height CONSTANT)

public:
    using QObject::QObject;

    QString photo_id = QStringLiteral("photo-a");
    QString representation_id = QStringLiteral("representation-a");
    QString visual_handle = QStringLiteral("visual-a");
    qulonglong decision_head_sequence = 0;
    QString decision_flag = QStringLiteral("unflagged");
    int decision_rating = 0;
    bool liked = false;
    QString color_label = QStringLiteral("none");
    QString title = QStringLiteral("Selected");
    QString source_path = QStringLiteral("/photos/selected.nef");
    bool source_available = true;
    QString visual_role = QStringLiteral("generated_proxy");
    QString visual_source = QStringLiteral("image://shadow/selected");
    int visual_width = 1'600;
    int visual_height = 1'200;
};

namespace {

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "review selection contract failed: " << message << '\n';
    }
    return condition;
}

[[nodiscard]] bool invoke(QObject* object, const char* method, QObject* argument) {
    return QMetaObject::invokeMethod(
        object,
        method,
        Q_ARG(QVariant, QVariant::fromValue(argument))
    );
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    QQmlEngine engine;
    const QString source_path = QStringLiteral(
        SHADOW_DESKTOP_SOURCE_DIR "/qml/ReviewSelectionState.qml"
    );
    if (!require(QFileInfo::exists(source_path), "production QML source is present")) {
        return EXIT_FAILURE;
    }
    QQmlComponent component(&engine, QUrl::fromLocalFile(source_path));
    FakeInspectionController controller;
    std::unique_ptr<QObject> selection(component.createWithInitialProperties({
        {QStringLiteral("controller"), QVariant::fromValue(&controller)},
    }));
    if (!selection) {
        std::cerr << component.errorString().toStdString();
        return EXIT_FAILURE;
    }

    auto card = std::make_unique<FakeReviewCard>();
    if (!require(
            invoke(selection.get(), "updatePrimaryPhoto", card.get()),
            "production selection function is invokable"
        )
        || !require(
            controller.requested_photo_id_ == card->photo_id
                && controller.requested_representation_id_
                    == card->representation_id,
            "selection requests its exact identity"
        )) {
        return EXIT_FAILURE;
    }
    card.reset();

    controller.publish({
        {QStringLiteral("available"), true},
        {QStringLiteral("photoId"), QStringLiteral("photo-a")},
        {
            QStringLiteral("representationId"),
            QStringLiteral("representation-a"),
        },
        {QStringLiteral("hasMetadata"), true},
        {QStringLiteral("cameraMake"), QStringLiteral("Nikon")},
    });
    QCoreApplication::processEvents();
    if (!require(
            selection->property("selectedCameraMake").toString()
                == QStringLiteral("Nikon"),
            "inspection updates after the originating card is destroyed"
        )) {
        return EXIT_FAILURE;
    }

    controller.publish({
        {QStringLiteral("available"), true},
        {QStringLiteral("photoId"), QStringLiteral("photo-a")},
        {
            QStringLiteral("representationId"),
            QStringLiteral("representation-b"),
        },
        {QStringLiteral("hasMetadata"), true},
        {QStringLiteral("cameraMake"), QStringLiteral("Wrong camera")},
    });
    QCoreApplication::processEvents();
    if (!require(
            selection->property("selectedCameraMake").toString().isEmpty(),
            "a mismatched representation is rejected by production QML"
        )
        || !require(
            QMetaObject::invokeMethod(selection.get(), "clearPrimaryPhoto"),
            "production clear function is invokable"
        )
        || !require(
            controller.clear_count_ == 1,
            "clearing selection clears the independent inspection request"
        )) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

#include "review_selection_state_contract_test.moc"
