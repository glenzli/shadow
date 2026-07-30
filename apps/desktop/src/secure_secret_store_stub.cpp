#include "secure_secret_store.hpp"

namespace {

class UnavailableSecretStore final : public SecretStore {
public:
    [[nodiscard]] bool available() const noexcept override {
        return false;
    }

    [[nodiscard]] SecretStoreResult read(
        const QString&,
        const QString&
    ) const override {
        return {
            .status = SecretStoreStatus::Unavailable,
            .diagnostic = QStringLiteral(
                "The operating system credential store is unavailable."
            ),
        };
    }

    [[nodiscard]] SecretStoreResult write(
        const QString&,
        const QString&,
        const QString&
    ) override {
        return {
            .status = SecretStoreStatus::Unavailable,
            .diagnostic = QStringLiteral(
                "The operating system credential store is unavailable."
            ),
        };
    }

    [[nodiscard]] SecretStoreResult remove(
        const QString&,
        const QString&
    ) override {
        return {
            .status = SecretStoreStatus::Unavailable,
            .diagnostic = QStringLiteral(
                "The operating system credential store is unavailable."
            ),
        };
    }
};

} // namespace

std::unique_ptr<SecretStore> makeSystemSecretStore() {
    return std::make_unique<UnavailableSecretStore>();
}
