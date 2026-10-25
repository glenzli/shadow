#pragma once

#include "storage_profile_archive.hpp"
#include "storage_profile_registry.hpp"
#include "storage_profile_switch.hpp"

#include <QObject>
#include <QTimer>
#include <QUrl>
#include <QVariantList>

#include <functional>

class StorageProfileController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString currentName READ currentName NOTIFY changed)
    Q_PROPERTY(QString currentRoot READ currentRoot CONSTANT)
    Q_PROPERTY(QString currentKind READ currentKind CONSTANT)
    Q_PROPERTY(bool debugLaunch READ debugLaunch CONSTANT)
    Q_PROPERTY(QVariantList recentProfiles READ recentProfiles NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool canCancel READ canCancel NOTIFY changed)
    Q_PROPERTY(QString statusText READ statusText NOTIFY changed)
    Q_PROPERTY(QString errorText READ errorText NOTIFY changed)
    Q_PROPERTY(QString resultRoot READ resultRoot NOTIFY changed)
  public:
    struct SwitchHooks final {
        // Return a product explanation when temporary authored state needs a
        // deliberate action (for example saving a composed image).
        std::function<QString()> blocker;
        std::function<void(bool)> suspend_background;
        std::function<bool()> jobs_quiet;
        std::function<bool()> prepare_editor;
        std::function<void(const StorageProfileInfo&, const QStringList&)> restart;
        std::function<QString()> archive_blocker;
        std::function<void(const StorageProfileArchiveRequest&)> archive;
        std::function<void()> cancel_archive;
    };
    StorageProfileController(
        StorageProfile& current,
        StorageProfileRegistry& registry,
        QString context,
        SwitchHooks hooks,
        QObject* parent = nullptr
    );
    [[nodiscard]] QString currentName() const;
    [[nodiscard]] QString currentRoot() const;
    [[nodiscard]] QString currentKind() const;
    [[nodiscard]] bool debugLaunch() const;
    [[nodiscard]] QVariantList recentProfiles() const;
    [[nodiscard]] bool busy() const;
    [[nodiscard]] bool canCancel() const;
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] QString errorText() const;
    [[nodiscard]] QString resultRoot() const;
    Q_INVOKABLE void refresh();
    void rememberCurrent();
    Q_INVOKABLE void renameCurrent(const QString& name);
    Q_INVOKABLE void createLibrary(const QString& name, const QUrl& parent);
    Q_INVOKABLE void openLibrary(const QUrl& directory);
    Q_INVOKABLE void switchLibrary(const QString& root, const QString& expected_id);
    Q_INVOKABLE void cancelSwitch();
    Q_INVOKABLE void revealCurrent();
    Q_INVOKABLE void copyCurrentLocation();
    Q_INVOKABLE void backupLibrary(const QUrl& parent);
    Q_INVOKABLE void restoreLibrary(const QUrl& backup, const QUrl& parent, const QString& name);
    Q_INVOKABLE void createTestCopy(const QUrl& parent, const QString& name);
    Q_INVOKABLE void verifyBackup(const QUrl& backup);
    Q_INVOKABLE void revealResult();
    void archiveCompleted(const QString& root);
    void archiveFailed(const QString& diagnostic);
    void archiveProgress(quint64 completed_files, quint64 total_files);
    void editorReady();
    void editorSaveFailed(const QString& error);
    void restartActivated();
    void restartFailed(const QString& error);
  signals:
    void changed();
    void switched();

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    enum class State { Idle, WaitingJobs, Saving, Archiving, Starting, Leaving };
    void beginArchive(StorageProfileArchiveRequest request);
    void begin(const StorageProfileInfo& target);
    void advance();
    void fail(const QString& error);
    StorageProfile& current_;
    StorageProfileRegistry& registry_;
    QString context_;
    SwitchHooks hooks_;
    QVector<StorageProfileInfo> recent_;
    StorageProfileInfo target_;
    State state_ = State::Idle;
    QString error_;
    QTimer poll_;
    StorageProfileRestart restart_;
    std::optional<StorageProfileArchiveRequest> archive_;
    QString result_root_;
    QString result_text_;
    quint64 completed_files_ = 0;
    quint64 total_files_ = 0;
    bool archive_cancelled_ = false;
};
