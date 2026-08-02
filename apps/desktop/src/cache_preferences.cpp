#include "cache_preferences.hpp"

#include <QDir>
#include <QSettings>
#include <QVariant>

#include <algorithm>

namespace {

constexpr auto disk_limit_gib_key = "cache/disk_limit_gib";
constexpr auto automatic_cleanup_allowed_key = "cache/automatic_cleanup_allowed";
constexpr qulonglong bytes_per_gib = 1024ULL * 1024ULL * 1024ULL;

[[nodiscard]] int normalize_limit(const int limit_gib) noexcept {
    if (limit_gib <= 0) {
        return 0;
    }
    return std::clamp(limit_gib, 1, 1000);
}

} // namespace

CachePreferences::CachePreferences(
    const QString& cache_root,
    const QString& isolated_settings_file,
    QObject* const parent
) :
    QObject(parent),
    settings_(
        isolated_settings_file.isEmpty()
            ? std::make_unique<QSettings>()
            : std::make_unique<QSettings>(isolated_settings_file, QSettings::IniFormat)
    ),
    cache_root_path_(QDir::cleanPath(cache_root)) {
    disk_limit_gib_ = normalize_limit(
        settings_->value(QString::fromLatin1(disk_limit_gib_key), disk_limit_gib_).toInt()
    );
    automatic_cleanup_allowed_ =
        settings_
            ->value(QString::fromLatin1(automatic_cleanup_allowed_key), automatic_cleanup_allowed_)
            .toBool();
}

CachePreferences::~CachePreferences() = default;

int CachePreferences::diskLimitGiB() const noexcept {
    return disk_limit_gib_;
}

bool CachePreferences::automaticCleanupAllowed() const noexcept {
    return automatic_cleanup_allowed_;
}

qulonglong CachePreferences::diskLimitBytes() const noexcept {
    if (disk_limit_gib_ == 0) {
        return 0;
    }
    return static_cast<qulonglong>(disk_limit_gib_) * bytes_per_gib;
}

QString CachePreferences::cacheRootPath() const {
    return cache_root_path_;
}

QUrl CachePreferences::cacheRootUrl() const {
    return QUrl::fromLocalFile(cache_root_path_);
}

void CachePreferences::setDiskLimitGiB(const int limit_gib) {
    const int normalized = normalize_limit(limit_gib);
    if (disk_limit_gib_ == normalized) {
        return;
    }
    disk_limit_gib_ = normalized;
    persist(disk_limit_gib_key, normalized);
    emit diskLimitGiBChanged();
}

void CachePreferences::setAutomaticCleanupAllowed(const bool allowed) {
    if (automatic_cleanup_allowed_ == allowed) {
        return;
    }
    automatic_cleanup_allowed_ = allowed;
    persist(automatic_cleanup_allowed_key, allowed);
    emit automaticCleanupAllowedChanged();
}

void CachePreferences::persist(const char* const key, const QVariant& value) {
    settings_->setValue(QString::fromLatin1(key), value);
    settings_->sync();
}
