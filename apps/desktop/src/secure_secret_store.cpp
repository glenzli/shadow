#include "secure_secret_store.hpp"

#include <QHash>

#include <utility>

namespace {

[[nodiscard]] QString secretIdentity(
    const QString& service,
    const QString& account
) {
    return service + QChar::Null + account;
}

class VolatileSecretStore final : public SecretStore {
public:
    [[nodiscard]] bool available() const noexcept override {
        return true;
    }

    [[nodiscard]] SecretStoreResult read(
        const QString& service,
        const QString& account
    ) const override {
        const auto found = secrets_.constFind(secretIdentity(service, account));
        if (found == secrets_.cend()) {
            return {.status = SecretStoreStatus::NotFound};
        }
        return {
            .status = SecretStoreStatus::Success,
            .value = found.value(),
        };
    }

    [[nodiscard]] SecretStoreResult write(
        const QString& service,
        const QString& account,
        const QString& value
    ) override {
        secrets_.insert(secretIdentity(service, account), value);
        return {.status = SecretStoreStatus::Success};
    }

    [[nodiscard]] SecretStoreResult remove(
        const QString& service,
        const QString& account
    ) override {
        const qsizetype removed = secrets_.remove(secretIdentity(service, account));
        return {
            .status = removed == 0
                ? SecretStoreStatus::NotFound
                : SecretStoreStatus::Success,
        };
    }

private:
    QHash<QString, QString> secrets_;
};

} // namespace

std::unique_ptr<SecretStore> makeVolatileSecretStore() {
    return std::make_unique<VolatileSecretStore>();
}
