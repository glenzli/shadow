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

/// Narrow platform-secret boundary used by desktop services.
///
/// Callers persist only stable service/account identifiers in ordinary
/// settings. Secret values remain in the platform credential store and are
/// never projected into QML properties.
class SecretStore {
public:
    virtual ~SecretStore() = default;

    [[nodiscard]] virtual bool available() const noexcept = 0;
    [[nodiscard]] virtual SecretStoreResult read(
        const QString& service,
        const QString& account
    ) const = 0;
    [[nodiscard]] virtual SecretStoreResult write(
        const QString& service,
        const QString& account,
        const QString& value
    ) = 0;
    [[nodiscard]] virtual SecretStoreResult remove(
        const QString& service,
        const QString& account
    ) = 0;
};

/// Returns the host credential-store implementation. Unsupported platforms
/// fail closed rather than writing secrets to a plaintext fallback.
[[nodiscard]] std::unique_ptr<SecretStore> makeSystemSecretStore();

/// Process-local store for isolated smoke tests and other explicitly
/// non-persistent sessions.
[[nodiscard]] std::unique_ptr<SecretStore> makeVolatileSecretStore();
