#pragma once

#include <QString>

#include <memory>

enum class SecretStoreStatus {
    Success,
    NotFound,
    Unavailable,
    Failure,
};

struct SecretStoreResult {
    SecretStoreStatus status = SecretStoreStatus::Failure;
    QString value;
    QString diagnostic;

    [[nodiscard]] bool succeeded() const noexcept {
        return status == SecretStoreStatus::Success;
    }
};

/// Narrow local-credential boundary used by desktop services.
///
/// Credentials are stored in a Shadow-owned, user-private file selected by
/// the application. Values are never projected into QML properties.
class SecretStore {
  public:
    virtual ~SecretStore() = default;

    [[nodiscard]] virtual bool available() const noexcept = 0;
    [[nodiscard]] virtual SecretStoreResult
    read(const QString& service, const QString& account) const = 0;
    [[nodiscard]] virtual SecretStoreResult
    write(const QString& service, const QString& account, const QString& value) = 0;
    [[nodiscard]] virtual SecretStoreResult
    remove(const QString& service, const QString& account) = 0;
};

/// Returns a persistent Shadow-local credential store. The containing
/// directory and resulting file are restricted to the current user.
[[nodiscard]] std::unique_ptr<SecretStore> makeLocalSecretStore(const QString& storage_file);

/// Process-local store for isolated smoke tests and other explicitly
/// non-persistent sessions.
[[nodiscard]] std::unique_ptr<SecretStore> makeVolatileSecretStore();
