#pragma once

#include "backend/remote_library_types.hpp"
#include "remote_photo_aggregation.hpp"
#include "remote_library_connection_store.hpp"
#include "review_model.hpp"
#include "secure_secret_store.hpp"

#include <QFutureWatcher>
#include <QHash>
#include <QObject>
#include <QQueue>
#include <QString>
#include <QVariantList>

#include <cstdint>
#include <functional>
#include <memory>

/// Owns the desktop remote-Library client lifecycle.
///
/// Cached snapshot reads, LAN synchronization, and RAW materialization all run
/// away from the UI thread. The coordinator projects only verified client-local
/// proxies into ReviewModel and emits a normal local identity after a verified
/// original is ready for Precision.
class ReviewRemoteLibraryCoordinator final : public QObject {
    Q_OBJECT

  public:
    struct Operations final {
        std::function<BackendRemoteLibrarySnapshot(const QString&)> snapshot;
        std::function<
            BackendRemoteLibrarySyncResult(const QString&, const QString&, const QString&)>
            sync;
        std::function<void(
            const QString&,
            const QString&,
            const QString&,
            BackendReviewDecisionFlag,
            std::uint8_t,
            bool,
            const QString&,
            std::int64_t
        )>
            set_review_state;
        std::function<BackendRemoteLibraryMaterialization(
            const QString&,
            const QString&,
            const QString&,
            const QString&,
            const QString&
        )>
            materialize;
    };

    explicit ReviewRemoteLibraryCoordinator(
        Operations operations,
        ReviewModel& model,
        const QString& isolated_settings_file,
        std::unique_ptr<SecretStore> secret_store,
        QObject* parent = nullptr
    );
    ~ReviewRemoteLibraryCoordinator() override;

    ReviewRemoteLibraryCoordinator(const ReviewRemoteLibraryCoordinator&) = delete;
    ReviewRemoteLibraryCoordinator& operator=(const ReviewRemoteLibraryCoordinator&) = delete;

    void start();
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] bool syncing() const noexcept;
    [[nodiscard]] bool materializing() const noexcept;
    [[nodiscard]] bool secureStorageAvailable() const noexcept;
    [[nodiscard]] bool tokenStored() const noexcept;
    [[nodiscard]] QVariantList connections() const;
    [[nodiscard]] QString serverAddress() const;
    [[nodiscard]] bool hasServer() const noexcept;
    [[nodiscard]] QString serverName() const;
    [[nodiscard]] int remotePhotoCount() const noexcept;
    [[nodiscard]] QString statusCode() const;
    [[nodiscard]] QString diagnosticText() const;
    [[nodiscard]] QString materializingPhotoId() const;
    [[nodiscard]] bool ownsPresentationPhoto(const QString& presentation_photo_id) const;

    [[nodiscard]] QString saveConnection(
        const QString& connection_id,
        const QString& server_address,
        const QString& token
    );
    [[nodiscard]] bool saveConnection(const QString& server_address, const QString& token);
    [[nodiscard]] bool removeConnection(const QString& connection_id);
    [[nodiscard]] bool removeConnection();
    void syncNow(const QString& connection_id);
    void syncNow();
    void syncAll();
    void reapplyRemoteItems();
    void materializeForEdit(const QString& presentation_photo_id);
    [[nodiscard]] bool
    setDecision(const QString& presentation_photo_id, BackendReviewDecisionFlag flag, int rating);
    [[nodiscard]] bool
    setAffinity(const QString& presentation_photo_id, bool liked, const QString& color_label);

  signals:
    void stateChanged();
    void connectionChanged();
    void remotePhotoReady(
        const QString& photoId,
        const QString& representationId,
        const QString& sourcePath,
        const QString& title
    );
    void localLibraryRefreshRequested();

  private:
    enum class SnapshotTaskKind : std::uint8_t {
        LoadCached,
        Sync,
    };

    struct SnapshotTaskResult final {
        SnapshotTaskKind kind = SnapshotTaskKind::LoadCached;
        QString connection_id;
        BackendRemoteLibrarySnapshot snapshot;
        BackendRemoteLibrarySyncResult sync_result;
        QString error;
    };

    struct MaterializeTaskResult final {
        QString connection_id;
        QString presentation_photo_id;
        BackendRemoteLibraryMaterialization materialization;
        QString error;
    };

    struct MutationRequest final {
        QString connection_id;
        QString presentation_photo_id;
        QString remote_photo_id;
        QString remote_representation_id;
        BackendReviewDecisionFlag flag = BackendReviewDecisionFlag::Unflagged;
        std::uint8_t rating = 0;
        bool liked = false;
        QString color_label = QStringLiteral("none");
        std::int64_t updated_at_ms = 0;
    };

    struct MutationTaskResult final {
        QString presentation_photo_id;
        QString error;
    };

    [[nodiscard]] static SnapshotTaskResult runSnapshotTask(
        Operations operations,
        SnapshotTaskKind kind,
        QString connection_id,
        QString server_address,
        QString authorization
    );
    [[nodiscard]] static MaterializeTaskResult runMaterializeTask(
        Operations operations,
        QString connection_id,
        QString presentation_photo_id,
        QString server_address,
        QString authorization,
        QString remote_photo_id,
        QString remote_representation_id
    );
    [[nodiscard]] static MutationTaskResult
    runMutationTask(Operations operations, MutationRequest request);

    void finishSnapshotTask();
    void finishMaterializeTask();
    void finishMutationTask();
    void startNextSnapshotTask();
    void startMutationIfIdle();
    void applySnapshot(const QString& connection_id, BackendRemoteLibrarySnapshot snapshot);
    [[nodiscard]] QVector<ReviewItem> projectedRemoteItems() const;
    [[nodiscard]] SecretStoreResult readAuthorization(const QString& connection_id) const;
    [[nodiscard]] const RemoteLibraryConnection* connection(const QString& connection_id) const;
    [[nodiscard]] QString tokenAccount(const RemoteLibraryConnection& connection) const;
    void migrateLegacySecret();
    void rebuildPhotoAggregates();
    void setStatus(const QString& code, const QString& diagnostic = {});
    void setConnectionStatus(
        const QString& connection_id,
        const QString& code,
        const QString& diagnostic = {}
    );
    [[nodiscard]] bool enqueueMutation(MutationRequest request);
    [[nodiscard]] static QString presentationRepresentationId(
        const QString& presentation_photo_id,
        const BackendRemoteLibraryPhoto& photo
    );

    Operations operations_;
    ReviewModel* model_;
    RemoteLibraryConnectionStore connection_store_;
    std::unique_ptr<SecretStore> secret_store_;
    QHash<QString, BackendRemoteLibrarySnapshot> snapshots_;
    RemotePhotoAggregateMap photo_aggregates_;
    QHash<QString, BackendRemoteLibraryPhoto> photos_;
    QHash<QString, QString> photo_connection_ids_;
    QHash<QString, QString> connection_status_codes_;
    QHash<QString, QString> connection_diagnostics_;
    QString status_code_;
    QString diagnostic_text_;
    QString active_snapshot_connection_id_;
    QString materializing_connection_id_;
    QString materializing_photo_id_;
    bool mutation_task_active_ = false;
    bool started_ = false;
    struct SnapshotRequest final {
        SnapshotTaskKind kind = SnapshotTaskKind::LoadCached;
        QString connection_id;
        QString server_address;
        QString authorization;
    };
    QQueue<SnapshotRequest> snapshot_queue_;
    QQueue<MutationRequest> mutation_queue_;
    QFutureWatcher<SnapshotTaskResult> snapshot_watcher_;
    QFutureWatcher<MaterializeTaskResult> materialize_watcher_;
    QFutureWatcher<MutationTaskResult> mutation_watcher_;
};
