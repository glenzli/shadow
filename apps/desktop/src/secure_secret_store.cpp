#include "secure_secret_store.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSettings>

#include <utility>

namespace {

[[nodiscard]] QString secretIdentity(const QString& service, const QString& account) {
    return service + QChar::Null + account;
}

[[nodiscard]] QString persistentSecretKey(const QString& service, const QString& account) {
    const QByteArray identity = secretIdentity(service, account).toUtf8();
    return QStringLiteral("credentials/")
           + QString::fromLatin1(
               QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex()
           );
}

[[nodiscard]] SecretStoreResult settingsFailure(const QSettings& settings) {
    return {
        .status = SecretStoreStatus::Failure,
        .diagnostic = settings.status() == QSettings::AccessError
                          ? QStringLiteral("Shadow cannot access its local credential file.")
                          : QStringLiteral("Shadow's local credential file is invalid."),
    };
}

class LocalSecretStore final : public SecretStore {
  public:
    explicit LocalSecretStore(QString storage_file) : storage_file_(std::move(storage_file)) {
        const QFileInfo info(storage_file_);
        available_ = !storage_file_.isEmpty() && QDir().mkpath(info.absolutePath());
    }

    [[nodiscard]] bool available() const noexcept override {
        return available_;
    }

    [[nodiscard]] SecretStoreResult
    read(const QString& service, const QString& account) const override {
        if (!available_) {
            return {.status = SecretStoreStatus::Unavailable};
        }
        QSettings settings(storage_file_, QSettings::IniFormat);
        if (settings.status() != QSettings::NoError) {
            return settingsFailure(settings);
        }
        const QString key = persistentSecretKey(service, account);
        if (!settings.contains(key)) {
            return {.status = SecretStoreStatus::NotFound};
        }
        return {
            .status = SecretStoreStatus::Success,
            .value = settings.value(key).toString(),
        };
    }

    [[nodiscard]] SecretStoreResult
    write(const QString& service, const QString& account, const QString& value) override {
        if (!available_) {
            return {.status = SecretStoreStatus::Unavailable};
        }
        QSettings settings(storage_file_, QSettings::IniFormat);
        settings.setValue(persistentSecretKey(service, account), value);
        settings.sync();
        if (settings.status() != QSettings::NoError) {
            return settingsFailure(settings);
        }
        if (!QFile::setPermissions(
                storage_file_,
                QFileDevice::ReadOwner | QFileDevice::WriteOwner
            )) {
            return {
                .status = SecretStoreStatus::Failure,
                .diagnostic = QStringLiteral(
                    "Shadow could not restrict its local credential file to this user."
                ),
            };
        }
        return {.status = SecretStoreStatus::Success};
    }

    [[nodiscard]] SecretStoreResult
    remove(const QString& service, const QString& account) override {
        if (!available_) {
            return {.status = SecretStoreStatus::Unavailable};
        }
        QSettings settings(storage_file_, QSettings::IniFormat);
        const QString key = persistentSecretKey(service, account);
        if (!settings.contains(key)) {
            return {.status = SecretStoreStatus::NotFound};
        }
        settings.remove(key);
        settings.sync();
        return settings.status() == QSettings::NoError
                   ? SecretStoreResult{.status = SecretStoreStatus::Success}
                   : settingsFailure(settings);
    }

  private:
    QString storage_file_;
    bool available_ = false;
};

class VolatileSecretStore final : public SecretStore {
  public:
    [[nodiscard]] bool available() const noexcept override {
        return true;
    }

    [[nodiscard]] SecretStoreResult
    read(const QString& service, const QString& account) const override {
        const auto found = secrets_.constFind(secretIdentity(service, account));
        if (found == secrets_.cend()) {
            return {.status = SecretStoreStatus::NotFound};
        }
        return {
            .status = SecretStoreStatus::Success,
            .value = found.value(),
        };
    }

    [[nodiscard]] SecretStoreResult
    write(const QString& service, const QString& account, const QString& value) override {
        secrets_.insert(secretIdentity(service, account), value);
        return {.status = SecretStoreStatus::Success};
    }

    [[nodiscard]] SecretStoreResult
    remove(const QString& service, const QString& account) override {
        const qsizetype removed = secrets_.remove(secretIdentity(service, account));
        return {
            .status = removed == 0 ? SecretStoreStatus::NotFound : SecretStoreStatus::Success,
        };
    }

  private:
    QHash<QString, QString> secrets_;
};

} // namespace

std::unique_ptr<SecretStore> makeLocalSecretStore(const QString& storage_file) {
    return std::make_unique<LocalSecretStore>(storage_file);
}

std::unique_ptr<SecretStore> makeVolatileSecretStore() {
    return std::make_unique<VolatileSecretStore>();
}
