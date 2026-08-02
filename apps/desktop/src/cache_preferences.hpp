#pragma once

#include <QObject>
#include <QString>
#include <QUrl>

#include <memory>

class QSettings;
class QVariant;

/// Persistent resource policy for the rebuildable local cache.
///
/// The limit is intentionally a soft target: automatic maintenance may remove
/// only entries the Catalog-backed maintenance service already classifies as
/// safe. Live or unknown entries can therefore keep actual usage above it.
class CachePreferences final : public QObject {
    Q_OBJECT
    Q_PROPERTY(int diskLimitGiB READ diskLimitGiB WRITE setDiskLimitGiB NOTIFY diskLimitGiBChanged)
    Q_PROPERTY(
        bool automaticCleanupAllowed READ automaticCleanupAllowed WRITE setAutomaticCleanupAllowed
            NOTIFY automaticCleanupAllowedChanged
    )
    Q_PROPERTY(qulonglong diskLimitBytes READ diskLimitBytes NOTIFY diskLimitGiBChanged)
    Q_PROPERTY(QString cacheRootPath READ cacheRootPath CONSTANT)
    Q_PROPERTY(QUrl cacheRootUrl READ cacheRootUrl CONSTANT)

  public:
    explicit CachePreferences(
        const QString& cache_root,
        const QString& isolated_settings_file = {},
        QObject* parent = nullptr
    );
    ~CachePreferences() override;

    CachePreferences(const CachePreferences&) = delete;
    CachePreferences& operator=(const CachePreferences&) = delete;

    [[nodiscard]] int diskLimitGiB() const noexcept;
    [[nodiscard]] bool automaticCleanupAllowed() const noexcept;
    [[nodiscard]] qulonglong diskLimitBytes() const noexcept;
    [[nodiscard]] QString cacheRootPath() const;
    [[nodiscard]] QUrl cacheRootUrl() const;

    void setDiskLimitGiB(int limit_gib);
    void setAutomaticCleanupAllowed(bool allowed);

  signals:
    void diskLimitGiBChanged();
    void automaticCleanupAllowedChanged();

  private:
    void persist(const char* key, const QVariant& value);

    std::unique_ptr<QSettings> settings_;
    QString cache_root_path_;
    int disk_limit_gib_ = 20;
    bool automatic_cleanup_allowed_ = false;
};
