#pragma once

#include <QString>
#include <QVector>

#include <memory>

class QSettings;

struct RemoteLibraryConnection final {
    QString id;
    QString address;
    bool token_stored = false;
    bool uses_legacy_secret = false;
};

/// Durable, non-secret registry for remote Library sources.
///
/// Network state and credentials remain with the coordinator and platform
/// secret store. This owner preserves stable connection identities and order,
/// including one-time migration of the former single-connection settings.
class RemoteLibraryConnectionStore final {
  public:
    explicit RemoteLibraryConnectionStore(const QString& isolated_settings_file = {});
    explicit RemoteLibraryConnectionStore(std::unique_ptr<QSettings> settings);
    ~RemoteLibraryConnectionStore();

    [[nodiscard]] const QVector<RemoteLibraryConnection>& connections() const noexcept;
    [[nodiscard]] QString add(const QString& address, bool token_stored);
    [[nodiscard]] bool
    update(const QString& connection_id, const QString& address, bool token_stored);
    [[nodiscard]] bool remove(const QString& connection_id);
    [[nodiscard]] bool setTokenStored(const QString& connection_id, bool stored);
    [[nodiscard]] bool finishLegacySecretMigration(const QString& connection_id);

  private:
    void load();
    void persist();
    [[nodiscard]] qsizetype indexOf(const QString& connection_id) const;

    std::unique_ptr<QSettings> settings_;
    QVector<RemoteLibraryConnection> connections_;
};
