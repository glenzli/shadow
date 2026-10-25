#pragma once

#include "storage_profile.hpp"

#include <QJsonObject>
#include <QVector>

// Bootstrap metadata only: locations, recent identities and two launch lanes.
// Catalogs, edit attachments, preferences and job state never enter this file.
class StorageProfileRegistry final {
    Q_DECLARE_TR_FUNCTIONS(StorageProfileRegistry)
  public:
    StorageProfileRegistry(QString daily_root, QString registry_root);
    [[nodiscard]] QString dailyRoot() const;
    [[nodiscard]] QString registryRoot() const;
    static QString launchContext(const StorageProfileLaunch& launch, QString* error);
    [[nodiscard]] std::optional<StorageProfileLaunch>
    resolveLaunch(const StorageProfileLaunch& launch, const QString& context, QString* error) const;
    [[nodiscard]] QVector<StorageProfileInfo> recent(QString* error) const;
    [[nodiscard]] QStringList argumentsFor(const StorageProfileInfo& info) const;
    [[nodiscard]] std::optional<StorageProfileInfo>
    prepare(const QString& root, const QString& expected_id, QString* error) const;
    [[nodiscard]] std::optional<StorageProfileInfo>
    create(const QString& name, const QString& parent, QString* error) const;
    bool remember(const StorageProfileInfo& info, const QString& context, QString* error) const;
    bool resetRecentRecords(QString* error) const;

  private:
    bool read(QJsonObject* document, QString* error) const;
    bool locationAllowed(
        const QString& root,
        const QVector<StorageProfileInfo>& known,
        QString* error
    ) const;
    QString daily_root_;
    QString registry_root_;
};
