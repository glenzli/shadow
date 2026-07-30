#pragma once

#include "desktop_backend.hpp"
#include "localized_ui_message.hpp"

#include <QFutureWatcher>
#include <QObject>
#include <QStringList>
#include <QVariantList>

#include <cstdint>
#include <functional>

/// Owns the complete Library keyword projection and mutation lifecycle.
///
/// One serialized worker owns taxonomy refresh, selected-photo assignments,
/// batch normalization, mutations, stale selection rejection, status, and
/// destruction wait. ReviewController remains only the stable QML facade.
class ReviewLibraryKeywordCoordinator final : public QObject {
    Q_OBJECT

  public:
    struct Operations final {
        std::function<QVector<BackendLibraryKeyword>()> tree;
        std::function<QVector<BackendLibraryPhotoKeyword>(const QString& photo_id)> for_photo;
        std::function<void(const QString& parent_id, const QString& name)> create;
        std::function<void(const QString& keyword_id, const QString& name)> rename;
        std::function<void(const QString& keyword_id, const QString& parent_id)> move;
        std::function<BackendLibraryKeywordDeletionReceipt(const QString& keyword_id)> remove;
        std::function<BackendLibraryKeywordMutationReceipt(
            const QString& keyword_id,
            const QStringList& photo_ids
        )>
            assign;
        std::function<BackendLibraryKeywordMutationReceipt(
            const QString& keyword_id,
            const QStringList& photo_ids
        )>
            unassign;
    };

    explicit ReviewLibraryKeywordCoordinator(Operations operations, QObject* parent = nullptr);
    ~ReviewLibraryKeywordCoordinator() override;

    [[nodiscard]] QVariantList keywords() const;
    [[nodiscard]] QVariantList photoKeywords() const;
    [[nodiscard]] QString photoId() const;
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] LocalizedUiMessage statusMessage() const;

    void setPhotoId(const QString& photo_id);
    void refresh();
    void createKeyword(const QString& parent_id, const QString& name);
    void renameKeyword(const QString& keyword_id, const QString& name);
    void moveKeyword(const QString& keyword_id, const QString& parent_id);
    void deleteKeyword(const QString& keyword_id);
    void assignKeyword(const QString& keyword_id, const QVariantList& targets);
    void removeKeyword(const QString& keyword_id, const QVariantList& targets);
    void retranslateUi();

  signals:
    void keywordsChanged();
    void photoKeywordsChanged();
    void keywordMutationAccepted();
    void statusMessageChanged();

  private:
    enum class TaskAction : std::uint8_t {
        Refresh,
        Create,
        Rename,
        Move,
        Delete,
        Assign,
        Unassign,
    };

    struct TaskResult final {
        QVector<BackendLibraryKeyword> keywords;
        QVector<BackendLibraryPhotoKeyword> photo_keywords;
        QString selected_photo_id;
        QString error;
        quint64 request_id = 0;
        TaskAction action = TaskAction::Refresh;
        std::uint64_t changed_photo_count = 0;
        std::uint64_t deleted_keyword_count = 0;
        bool has_snapshot = false;
    };

    [[nodiscard]] static QStringList photoIds(const QVariantList& targets);
    [[nodiscard]] static TaskResult runTask(
        Operations operations,
        TaskAction action,
        QString keyword_id,
        QString parent_id,
        QString name,
        QStringList photo_ids,
        QString selected_photo_id,
        quint64 request_id
    );

    void startTask(
        TaskAction action,
        const QString& keyword_id = {},
        const QString& parent_id = {},
        const QString& name = {},
        const QStringList& photo_ids = {}
    );
    void finishTask();
    void publishSuccess(const TaskResult& result);
    void setStatus(LocalizedUiMessage status);

    Operations operations_;
    bool task_running_ = false;
    bool refresh_pending_ = false;
    quint64 request_id_ = 0;
    quint64 active_request_id_ = 0;
    QString photo_id_;
    QVector<BackendLibraryKeyword> keywords_;
    QVector<BackendLibraryPhotoKeyword> photo_keywords_;
    LocalizedUiMessage status_message_;
    QFutureWatcher<TaskResult> watcher_;
};
