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
    Q_PROPERTY(QVariantMap photoInspection READ photoInspection NOTIFY photoInspectionChanged)
    Q_PROPERTY(bool photoInspectionBusy READ photoInspectionBusy NOTIFY photoInspectionChanged)
    Q_PROPERTY(bool photoInspectionFailed READ photoInspectionFailed NOTIFY photoInspectionChanged)

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

    Q_INVOKABLE void
    requestPhotoInspection(const QString& photo_id, const QString& representation_id) {
        requested_photo_id_ = photo_id;
        requested_representation_id_ = representation_id;
    }

    Q_INVOKABLE void clearPhotoInspection() {
        ++clear_count_;
        inspection_.clear();
        emit photoInspectionChanged();
    }

    Q_INVOKABLE void requestFocusDetail(
        const QString& photo_id,
        const QString& source_path,
        const double center_x,
        const double center_y
    ) {
        focus_photo_id_ = photo_id;
        focus_source_path_ = source_path;
        focus_center_x_ = center_x;
        focus_center_y_ = center_y;
    }

    Q_INVOKABLE void clearFocusDetail() {
        ++focus_clear_count_;
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
    QString focus_photo_id_;
    QString focus_source_path_;
    double focus_center_x_ = 0.0;
    double focus_center_y_ = 0.0;
    int focus_clear_count_ = 0;

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
    Q_PROPERTY(QString locationId MEMBER location_id CONSTANT)
    Q_PROPERTY(QString sourcePath MEMBER source_path CONSTANT)
    Q_PROPERTY(bool sourceAvailable MEMBER source_available CONSTANT)
    Q_PROPERTY(bool isRemote MEMBER is_remote CONSTANT)
    Q_PROPERTY(bool remoteOriginalCached MEMBER remote_original_cached CONSTANT)
    Q_PROPERTY(QString remoteConnectionId MEMBER remote_connection_id CONSTANT)
    Q_PROPERTY(
        QString remotePreviewUnavailableReason MEMBER remote_preview_unavailable_reason CONSTANT
    )
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
    QString location_id = QStringLiteral("location-a");
    QString source_path = QStringLiteral("/photos/selected.nef");
    bool source_available = true;
    bool is_remote = false;
    bool remote_original_cached = false;
    QString remote_connection_id;
    QString remote_preview_unavailable_reason;
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
    const QString source_path =
        QStringLiteral(SHADOW_DESKTOP_SOURCE_DIR "/qml/ReviewSelectionState.qml");
    if (!require(QFileInfo::exists(source_path), "production QML source is present")) {
        return EXIT_FAILURE;
    }
    QQmlComponent component(&engine, QUrl::fromLocalFile(source_path));
    FakeInspectionController controller;
    std::unique_ptr<QObject> selection(component.createWithInitialProperties({
        {QStringLiteral("controller"), QVariant::fromValue(&controller)},
        {QStringLiteral("focusDetailEnabled"), true},
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
                && controller.requested_representation_id_ == card->representation_id,
            "selection requests its exact identity"
        )
        || !require(
            selection->property("selectedLocationId").toString() == card->location_id,
            "selection snapshots the exact source location"
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
        {QStringLiteral("hasFocusObservation"), true},
        {QStringLiteral("focusObservationCenterX"), 0.25},
        {QStringLiteral("focusObservationCenterY"), 0.75},
    });
    QCoreApplication::processEvents();
    if (!require(
            selection->property("selectedCameraMake").toString() == QStringLiteral("Nikon"),
            "inspection updates after the originating card is destroyed"
        )
        || !require(
            controller.focus_photo_id_ == QStringLiteral("photo-a")
                && controller.focus_source_path_ == QStringLiteral("/photos/selected.nef")
                && controller.focus_center_x_ == 0.25 && controller.focus_center_y_ == 0.75,
            "focus detail requests the exact selected source and camera position"
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
        )) {
        return EXIT_FAILURE;
    }

    auto remote_card = std::make_unique<FakeReviewCard>();
    remote_card->photo_id = QStringLiteral("remote-photo");
    remote_card->representation_id = QStringLiteral("remote-representation");
    remote_card->is_remote = true;
    remote_card->remote_original_cached = true;
    remote_card->remote_connection_id = QStringLiteral("connection-a");
    remote_card->remote_preview_unavailable_reason = QStringLiteral("preview_cache_unavailable");
    if (!require(
            invoke(selection.get(), "updatePrimaryPhoto", remote_card.get()),
            "remote selection is invokable"
        )
        || !require(
            selection->property("selectedIsRemote").toBool()
                && selection->property("selectedRemoteOriginalCached").toBool()
                && selection->property("selectedRemoteConnectionId").toString()
                       == QStringLiteral("connection-a")
                && selection->property("selectedRemotePreviewUnavailableReason").toString()
                       == QStringLiteral("preview_cache_unavailable"),
            "remote origin, connection, preview availability, and original residency remain "
            "independent selection facts"
        )
        || !require(
            QMetaObject::invokeMethod(selection.get(), "clearPrimaryPhoto"),
            "production clear function is invokable"
        )
        || !require(
            controller.clear_count_ == 2,
            "clearing selection clears the independent inspection request"
        )) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

#include "review_selection_state_contract_test.moc"
