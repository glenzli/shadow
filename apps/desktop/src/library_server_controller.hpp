#pragma once

#include "backend/library_server_types.hpp"
#include "secure_secret_store.hpp"

#include <QFutureWatcher>
#include <QObject>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <functional>
#include <memory>
#include <optional>

class DesktopBackend;
class QSettings;

struct LibraryServerControllerOperations final {
    std::function<BackendLibraryServerSnapshot()> snapshot;
    std::function<BackendLibraryServerSnapshot(const BackendLibraryServerConfig&)> start;
    std::function<BackendLibraryServerSnapshot()> stop;
    std::function<BackendLibraryServerSnapshot()> reset_cache;
};

enum class LibraryServerTaskKind {
    Refresh,
    Start,
    Stop,
    Restart,
    ResetCache,
};

struct LibraryServerTaskResult final {
    LibraryServerTaskKind kind = LibraryServerTaskKind::Refresh;
    std::optional<BackendLibraryServerSnapshot> snapshot;
    QString error;
};

/// Owns this Mac's remote-Library configuration and listener lifecycle.
///
/// Folder scanning, Provider admission, and server threads remain in the Rust service. This
/// facade keeps persistent user intent, Keychain-only authorization, asynchronous admission, and
/// QML-safe state projection together.
class LibraryServerController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool running READ running NOTIFY stateChanged)
    Q_PROPERTY(bool secureStorageAvailable READ secureStorageAvailable CONSTANT)
    Q_PROPERTY(bool accessTokenStored READ accessTokenStored NOTIFY stateChanged)
    Q_PROPERTY(QString displayName READ displayName WRITE setDisplayName NOTIFY settingsChanged)
    Q_PROPERTY(int port READ port WRITE setPort NOTIFY settingsChanged)
    Q_PROPERTY(
        bool servesOriginals READ servesOriginals WRITE setServesOriginals NOTIFY settingsChanged
    )
    Q_PROPERTY(bool autoStart READ autoStart WRITE setAutoStart NOTIFY settingsChanged)
    Q_PROPERTY(QVariantList sharedFolders READ sharedFolders NOTIFY settingsChanged)
    Q_PROPERTY(QString localAddress READ localAddress NOTIFY stateChanged)
    Q_PROPERTY(QString providerMode READ providerMode NOTIFY stateChanged)
    Q_PROPERTY(qulonglong photoCount READ photoCount NOTIFY stateChanged)
    Q_PROPERTY(qulonglong cacheByteLength READ cacheByteLength NOTIFY stateChanged)
    Q_PROPERTY(QString statusCode READ statusCode NOTIFY stateChanged)
    Q_PROPERTY(QString diagnosticText READ diagnosticText NOTIFY stateChanged)

  public:
    explicit LibraryServerController(
        std::shared_ptr<DesktopBackend> backend,
        const QString& isolated_settings_file,
        std::unique_ptr<SecretStore> secret_store,
        QObject* parent = nullptr
    );
    LibraryServerController(
        LibraryServerControllerOperations operations,
        const QString& isolated_settings_file,
        std::unique_ptr<SecretStore> secret_store,
        QObject* parent = nullptr
    );
    ~LibraryServerController() override;

    LibraryServerController(const LibraryServerController&) = delete;
    LibraryServerController& operator=(const LibraryServerController&) = delete;
    LibraryServerController(LibraryServerController&&) = delete;
    LibraryServerController& operator=(LibraryServerController&&) = delete;

    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] bool secureStorageAvailable() const noexcept;
    [[nodiscard]] bool accessTokenStored() const noexcept;
    [[nodiscard]] QString displayName() const;
    [[nodiscard]] int port() const noexcept;
    [[nodiscard]] bool servesOriginals() const noexcept;
    [[nodiscard]] bool autoStart() const noexcept;
    [[nodiscard]] QVariantList sharedFolders() const;
    [[nodiscard]] QString localAddress() const;
    [[nodiscard]] QString providerMode() const;
    [[nodiscard]] qulonglong photoCount() const noexcept;
    [[nodiscard]] qulonglong cacheByteLength() const noexcept;
    [[nodiscard]] QString statusCode() const;
    [[nodiscard]] QString diagnosticText() const;

    void setDisplayName(const QString& value);
    void setPort(int value);
    void setServesOriginals(bool value);
    void setAutoStart(bool value);

    Q_INVOKABLE bool addSharedFolder(const QUrl& folder_url);
    Q_INVOKABLE bool removeSharedFolder(int index);
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void startServer();
    Q_INVOKABLE void stopServer();
    Q_INVOKABLE void rescanAndRestart();
    Q_INVOKABLE void clearCache();
    Q_INVOKABLE bool copyAccessToken();
    Q_INVOKABLE bool regenerateAccessToken();

  signals:
    void busyChanged();
    void stateChanged();
    void settingsChanged();

  private:
    void initialize();
    void loadTokenState();
    [[nodiscard]] bool ensureAccessToken();
    [[nodiscard]] SecretStoreResult readAccessToken() const;
    [[nodiscard]] BackendLibraryServerConfig currentConfig(const QString& token) const;
    void startTask(LibraryServerTaskKind kind, BackendLibraryServerConfig config = {});
    void finishTask();
    void applySnapshot(const BackendLibraryServerSnapshot& snapshot);
    void setStatus(const QString& code, const QString& diagnostic = {});
    void persistFolders();

    LibraryServerControllerOperations operations_;
    std::unique_ptr<QSettings> settings_;
    std::unique_ptr<SecretStore> secret_store_;
    QFutureWatcher<LibraryServerTaskResult> watcher_;
    QString display_name_;
    QStringList shared_folders_;
    int port_ = 37'641;
    bool serves_originals_ = true;
    bool auto_start_ = false;
    bool access_token_stored_ = false;
    BackendLibraryServerSnapshot snapshot_;
    QString status_code_;
    QString diagnostic_text_;
};
