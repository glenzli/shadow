#include "remote_library_connection_store.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSettings>
#include <QUuid>

#include <utility>

namespace {

constexpr auto connections_key = "remote_libraries/connections";
constexpr auto legacy_server_address_key = "remote_library/server_address";
constexpr auto legacy_token_stored_key = "remote_library/token_stored";

[[nodiscard]] std::unique_ptr<QSettings> makeSettings(const QString& isolated_settings_file) {
    if (isolated_settings_file.isEmpty()) {
        return std::make_unique<QSettings>();
    }
    return std::make_unique<QSettings>(isolated_settings_file, QSettings::IniFormat);
}

[[nodiscard]] QString newConnectionId() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

[[nodiscard]] bool validConnectionId(const QString& value) {
    return !QUuid::fromString(value).isNull();
}

} // namespace

RemoteLibraryConnectionStore::RemoteLibraryConnectionStore(const QString& isolated_settings_file) :
    RemoteLibraryConnectionStore(makeSettings(isolated_settings_file)) {}

RemoteLibraryConnectionStore::RemoteLibraryConnectionStore(std::unique_ptr<QSettings> settings) :
    settings_(std::move(settings)) {
    load();
}

RemoteLibraryConnectionStore::~RemoteLibraryConnectionStore() = default;

const QVector<RemoteLibraryConnection>& RemoteLibraryConnectionStore::connections() const noexcept {
    return connections_;
}

QString RemoteLibraryConnectionStore::add(const QString& address, const bool token_stored) {
    const QString id = newConnectionId();
    connections_.push_back({
        .id = id,
        .address = address.trimmed(),
        .token_stored = token_stored,
    });
    persist();
    return id;
}

bool RemoteLibraryConnectionStore::update(
    const QString& connection_id,
    const QString& address,
    const bool token_stored
) {
    const qsizetype index = indexOf(connection_id);
    if (index < 0) {
        return false;
    }
    auto& connection = connections_[index];
    connection.address = address.trimmed();
    connection.token_stored = token_stored;
    persist();
    return true;
}

bool RemoteLibraryConnectionStore::remove(const QString& connection_id) {
    const qsizetype index = indexOf(connection_id);
    if (index < 0) {
        return false;
    }
    connections_.removeAt(index);
    persist();
    return true;
}

bool RemoteLibraryConnectionStore::setTokenStored(const QString& connection_id, const bool stored) {
    const qsizetype index = indexOf(connection_id);
    if (index < 0) {
        return false;
    }
    connections_[index].token_stored = stored;
    persist();
    return true;
}

bool RemoteLibraryConnectionStore::finishLegacySecretMigration(const QString& connection_id) {
    const qsizetype index = indexOf(connection_id);
    if (index < 0) {
        return false;
    }
    connections_[index].uses_legacy_secret = false;
    settings_->remove(QString::fromLatin1(legacy_server_address_key));
    settings_->remove(QString::fromLatin1(legacy_token_stored_key));
    persist();
    return true;
}

void RemoteLibraryConnectionStore::load() {
    const QByteArray bytes = settings_->value(QString::fromLatin1(connections_key)).toByteArray();
    const QJsonDocument document = QJsonDocument::fromJson(bytes);
    if (document.isArray()) {
        QSet<QString> loaded_ids;
        for (const QJsonValue& value : document.array()) {
            const QJsonObject object = value.toObject();
            const QString id = object.value(QStringLiteral("id")).toString();
            const QString address = object.value(QStringLiteral("address")).toString().trimmed();
            if (!validConnectionId(id) || loaded_ids.contains(id) || address.isEmpty()
                || address.size() > 512) {
                continue;
            }
            loaded_ids.insert(id);
            connections_.push_back({
                .id = id,
                .address = address,
                .token_stored = object.value(QStringLiteral("tokenStored")).toBool(),
                .uses_legacy_secret = object.value(QStringLiteral("legacySecret")).toBool(),
            });
        }
    }
    if (!connections_.isEmpty()) {
        return;
    }

    const QString legacy_address =
        settings_->value(QString::fromLatin1(legacy_server_address_key)).toString().trimmed();
    if (legacy_address.isEmpty()) {
        return;
    }
    connections_.push_back({
        .id = newConnectionId(),
        .address = legacy_address,
        .token_stored =
            settings_->value(QString::fromLatin1(legacy_token_stored_key), false).toBool(),
        .uses_legacy_secret = true,
    });
    persist();
}

void RemoteLibraryConnectionStore::persist() {
    QJsonArray connections;
    for (const auto& connection : connections_) {
        connections.push_back(
            QJsonObject{
                {QStringLiteral("id"), connection.id},
                {QStringLiteral("address"), connection.address},
                {QStringLiteral("tokenStored"), connection.token_stored},
                {QStringLiteral("legacySecret"), connection.uses_legacy_secret},
            }
        );
    }
    settings_->setValue(
        QString::fromLatin1(connections_key),
        QJsonDocument(connections).toJson(QJsonDocument::Compact)
    );
    settings_->sync();
}

qsizetype RemoteLibraryConnectionStore::indexOf(const QString& connection_id) const {
    for (qsizetype index = 0; index < connections_.size(); ++index) {
        if (connections_[index].id == connection_id) {
            return index;
        }
    }
    return -1;
}
