#pragma once

#include "backend/history_types.hpp"
#include "history_model.hpp"
#include "localized_ui_message.hpp"

#include <QAbstractItemModel>
#include <QFutureWatcher>
#include <QObject>
#include <QString>
#include <QVariantList>

#include <cstdint>
#include <functional>

/// Owns lazy, asynchronous History browsing for photo and Library scopes.
/// Every request is generation-bound; stale photo responses cannot repopulate
/// a newly selected photo, and destruction waits for all active workers.
class HistoryCoordinator final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString photoId READ photoId NOTIFY photoChanged)
    Q_PROPERTY(QAbstractItemModel* photoModel READ photoModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel* libraryModel READ libraryModel CONSTANT)
    Q_PROPERTY(QVariantList libraryRefs READ libraryRefs NOTIFY libraryRefsChanged)
    Q_PROPERTY(bool photoBusy READ photoBusy NOTIFY photoStateChanged)
    Q_PROPERTY(bool libraryBusy READ libraryBusy NOTIFY libraryStateChanged)
    Q_PROPERTY(bool libraryRefsBusy READ libraryRefsBusy NOTIFY libraryRefsChanged)
    Q_PROPERTY(bool photoHasMore READ photoHasMore NOTIFY photoStateChanged)
    Q_PROPERTY(bool libraryHasMore READ libraryHasMore NOTIFY libraryStateChanged)
    Q_PROPERTY(bool libraryRefsHaveMore READ libraryRefsHaveMore NOTIFY libraryRefsChanged)
    Q_PROPERTY(QString photoErrorText READ photoErrorText NOTIFY photoStateChanged)
    Q_PROPERTY(QString libraryErrorText READ libraryErrorText NOTIFY libraryStateChanged)
    Q_PROPERTY(QString libraryRefsErrorText READ libraryRefsErrorText NOTIFY libraryRefsChanged)

  public:
    struct Operations final {
        std::function<BackendPhotoHistoryPage(
            const QString& photo_id,
            const BackendHistoryCursor& after,
            std::uint32_t limit
        )>
            photo_page;
        std::function<
            BackendLibraryHistoryPage(const BackendHistoryCursor& after, std::uint32_t limit)>
            library_page;
        std::function<BackendLibraryHistoryRefPage(const QString& after_name, std::uint32_t limit)>
            library_ref_page;
    };

    explicit HistoryCoordinator(Operations operations, QObject* parent = nullptr);
    ~HistoryCoordinator() override;

    [[nodiscard]] QString photoId() const;
    [[nodiscard]] QAbstractItemModel* photoModel() noexcept;
    [[nodiscard]] QAbstractItemModel* libraryModel() noexcept;
    [[nodiscard]] QVariantList libraryRefs() const;
    [[nodiscard]] bool photoBusy() const noexcept;
    [[nodiscard]] bool libraryBusy() const noexcept;
    [[nodiscard]] bool libraryRefsBusy() const noexcept;
    [[nodiscard]] bool photoHasMore() const noexcept;
    [[nodiscard]] bool libraryHasMore() const noexcept;
    [[nodiscard]] bool libraryRefsHaveMore() const noexcept;
    [[nodiscard]] QString photoErrorText() const;
    [[nodiscard]] QString libraryErrorText() const;
    [[nodiscard]] QString libraryRefsErrorText() const;

    Q_INVOKABLE void openForPhoto(const QString& photo_id);
    Q_INVOKABLE void refreshPhoto();
    Q_INVOKABLE void refreshLibrary();
    Q_INVOKABLE void refreshLibraryRefs();
    Q_INVOKABLE void refreshAll();
    Q_INVOKABLE void loadMorePhoto();
    Q_INVOKABLE void loadMoreLibrary();
    Q_INVOKABLE void loadMoreLibraryRefs();
    Q_INVOKABLE void retranslateUi();

  signals:
    void photoChanged();
    void photoStateChanged();
    void libraryStateChanged();
    void libraryRefsChanged();

  private:
    struct PhotoTaskResult final {
        BackendPhotoHistoryPage page;
        QString error;
        QString photo_id;
        quint64 generation = 0;
        quint64 request_id = 0;
        bool reset = false;
    };

    struct LibraryTaskResult final {
        BackendLibraryHistoryPage page;
        QString error;
        quint64 request_id = 0;
        bool reset = false;
    };

    struct RefTaskResult final {
        BackendLibraryHistoryRefPage page;
        QString error;
        quint64 request_id = 0;
        bool reset = false;
    };

    static PhotoTaskResult runPhotoTask(
        Operations operations,
        QString photo_id,
        BackendHistoryCursor cursor,
        quint64 generation,
        quint64 request_id,
        bool reset
    );
    static LibraryTaskResult runLibraryTask(
        Operations operations,
        BackendHistoryCursor cursor,
        quint64 request_id,
        bool reset
    );
    static RefTaskResult
    runRefTask(Operations operations, QString cursor, quint64 request_id, bool reset);

    void startPhoto(bool reset);
    void startLibrary(bool reset);
    void startRefs(bool reset);
    void finishPhoto();
    void finishLibrary();
    void finishRefs();
    void setPhotoError(const QString& diagnostic);
    void setLibraryError(const QString& diagnostic);
    void setRefError(const QString& diagnostic);

    Operations operations_;
    QString photo_id_;
    PhotoHistoryModel photo_model_;
    LibraryHistoryModel library_model_;
    QVector<BackendHistoryRef> library_refs_;
    BackendHistoryCursor photo_cursor_;
    BackendHistoryCursor library_cursor_;
    QString ref_cursor_;
    LocalizedUiMessage photo_error_;
    LocalizedUiMessage library_error_;
    LocalizedUiMessage ref_error_;
    quint64 photo_generation_ = 1;
    quint64 photo_request_id_ = 0;
    quint64 active_photo_request_id_ = 0;
    quint64 library_request_id_ = 0;
    quint64 active_library_request_id_ = 0;
    quint64 ref_request_id_ = 0;
    quint64 active_ref_request_id_ = 0;
    bool photo_running_ = false;
    bool library_running_ = false;
    bool ref_running_ = false;
    bool photo_reset_pending_ = false;
    bool library_reset_pending_ = false;
    bool ref_reset_pending_ = false;
    bool photo_has_more_ = false;
    bool library_has_more_ = false;
    bool refs_have_more_ = false;
    QFutureWatcher<PhotoTaskResult> photo_watcher_;
    QFutureWatcher<LibraryTaskResult> library_watcher_;
    QFutureWatcher<RefTaskResult> ref_watcher_;
};
