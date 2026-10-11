#include "storage_profile.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QSettings>
#include <QTemporaryFile>
#include <QUuid>

#include <algorithm>
#include <utility>

namespace {
constexpr qint64 maximum_settings_bytes = 16 * 1024 * 1024;

// Resolve existing ancestors as well as the final component. A not-yet-created
// directory beneath a symlink must not bypass the Daily/Debug separation.
QString resolvedPath(const QString& path) {
    QString ancestor = QDir::cleanPath(path);
    QStringList suffix;
    while (!QFileInfo::exists(ancestor) && !QFileInfo(ancestor).isSymLink()) {
        const QFileInfo info(ancestor);
        const QString parent = info.absolutePath();
        if (parent == ancestor)
            return {};
        suffix.prepend(info.fileName());
        ancestor = parent;
    }
    const QString canonical = QFileInfo(ancestor).canonicalFilePath();
    return canonical.isEmpty() ? QString{}
                               : QDir::cleanPath(QDir(canonical).filePath(suffix.join('/')));
}

bool containsPath(const QString& parent, const QString& child) {
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    // macOS realpath preserves spelling on common case-insensitive volumes.
    // Reserve case variants as well, including not-yet-created library roots.
    constexpr auto sensitivity = Qt::CaseInsensitive;
#else
    constexpr auto sensitivity = Qt::CaseSensitive;
#endif
    return !parent.isEmpty() && !child.isEmpty()
           && (child.compare(parent, sensitivity) == 0
               || child.startsWith(parent.endsWith('/') ? parent : parent + '/', sensitivity));
}

bool writeAtomically(const QString& path, const QByteArray& bytes) {
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}

bool boundedFileBytes(const QString& path, QByteArray* bytes) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > maximum_settings_bytes)
        return false;
    *bytes = file.readAll();
    return file.error() == QFileDevice::NoError;
}
} // namespace

StorageProfile::StorageProfile(QString root, QString kind, StorageProfileProcess process) :
    root_(std::move(root)), kind_(std::move(kind)), process_(process) {}
StorageProfile::~StorageProfile() = default;

QString StorageProfileInfo::name() const {
    if (!display_name.isEmpty())
        return display_name;
    if (kind == "daily")
        return QCoreApplication::translate("StorageProfile", "Daily Library");
    if (kind == "debug")
        return QCoreApplication::translate("StorageProfile", "Debug Library");
    return QFileInfo(root).fileName();
}

QString StorageProfile::canonicalRoot(const QString& path) {
    return QDir::isAbsolutePath(path) ? resolvedPath(path) : QString{};
}

bool StorageProfile::rootsOverlap(const QString& left, const QString& right) {
    return containsPath(left, right) || containsPath(right, left);
}

bool StorageProfile::validDisplayName(const QString& name) {
    return !name.trimmed().isEmpty() && name.size() <= 80
           && std::none_of(name.cbegin(), name.cend(), [](const QChar ch) {
                  return ch.isNull() || ch.category() == QChar::Other_Control;
              });
}

std::optional<StorageProfileInfo> StorageProfile::inspect(
    const QString& requested_root,
    const QString& daily_root,
    QString* const error
) {
    const QString root = canonicalRoot(requested_root);
    const QString daily = canonicalRoot(daily_root);
    if (root.isEmpty() || daily.isEmpty() || !QFileInfo(root).isDir()) {
        *error = tr("The library directory is unavailable: %1").arg(requested_root);
        return std::nullopt;
    }
    const QString manifest_path = QDir(root).filePath("storage-profile.json");
    if (canonicalRoot(manifest_path) != manifest_path) {
        *error = tr("Profile storage must remain inside its directory: %1").arg(manifest_path);
        return std::nullopt;
    }
    QByteArray bytes;
    if (!boundedFileBytes(manifest_path, &bytes)) {
        *error = tr("Choose a Shadow library directory containing storage-profile.json.");
        return std::nullopt;
    }
    const auto manifest = QJsonDocument::fromJson(bytes).object();
    StorageProfileInfo info{
        root,
        manifest.value("kind").toString(),
        manifest.value("id").toString(),
        manifest.value("displayName").toString()
    };
    const QString expected_kind = root == daily              ? "daily"
                                  : root == daily + "-debug" ? "debug"
                                                             : "custom";
    if (manifest.value("schemaVersion").toInt() != 1 || QUuid(info.id).isNull()
        || info.kind != expected_kind || (info.kind != "daily" && rootsOverlap(daily, root))
        || (manifest.contains("displayName") && !manifest.value("displayName").isString())
        || (!info.display_name.isEmpty() && !validDisplayName(info.display_name))) {
        *error = tr("The storage profile is incompatible with this selection: %1").arg(root);
        return std::nullopt;
    }
    return info;
}

StorageProfileInfo StorageProfile::info() const {
    return {root_, kind_, id_, display_name_};
}

bool StorageProfile::rename(const QString& name, QString* const error) {
    const QString normalized = name.trimmed();
    if (!validDisplayName(normalized)) {
        *error = tr("Use a library name of 1–80 characters without control characters.");
        return false;
    }
    const QString lock_path = QDir(root_).filePath("storage-profile-initialization.lock");
    if (canonicalRoot(lock_path) != lock_path) {
        *error = tr("Profile storage must remain inside its directory: %1").arg(lock_path);
        return false;
    }
    QLockFile enrollment(lock_path);
    enrollment.setStaleLockTime(0);
    if (!enrollment.tryLock()) {
        *error = tr("Could not initialize the profile directory: %1").arg(root_);
        return false;
    }
    const QString path = QDir(root_).filePath("storage-profile.json");
    QByteArray bytes;
    if (canonicalRoot(path) != path || !boundedFileBytes(path, &bytes)) {
        *error = tr("Could not read the storage profile: %1").arg(path);
        return false;
    }
    auto manifest = QJsonDocument::fromJson(bytes).object();
    if (manifest.value("schemaVersion").toInt() != 1 || manifest.value("id").toString() != id_
        || manifest.value("kind").toString() != kind_) {
        *error = tr("The storage profile is incompatible with this selection: %1").arg(root_);
        return false;
    }
    manifest.insert("displayName", normalized);
    if (!writeAtomically(path, QJsonDocument(manifest).toJson())) {
        *error = tr("Could not save the storage profile: %1").arg(path);
        return false;
    }
    display_name_ = normalized;
    return true;
}

std::unique_ptr<StorageProfile>
StorageProfile::open(const StorageProfileLaunch& launch, QString* const error) {
    QString selection;
    QString expected_id;
    QString requested_root;
    bool root_option = false;
    for (qsizetype index = 1; index < launch.arguments.size(); ++index) {
        const QString argument = launch.arguments[index];
        const bool profile = argument == "--profile" || argument.startsWith("--profile=");
        const bool root = argument == "--profile-root" || argument.startsWith("--profile-root=");
        const bool identity = argument == "--profile-id" || argument.startsWith("--profile-id=");
        if (!profile && !root && !identity)
            continue;
        QString value;
        if (argument.contains('=')) {
            value = argument.sliced(argument.indexOf('=') + 1);
        } else if (
            index + 1 < launch.arguments.size() && !launch.arguments[index + 1].startsWith("--")
        ) {
            value = launch.arguments[++index];
        }
        if (identity) {
            if (!expected_id.isEmpty() || QUuid(value).isNull()) {
                *error = tr("The library identity is invalid.");
                return nullptr;
            }
            expected_id = value;
            continue;
        }
        if (value.isEmpty() || (profile && !selection.isEmpty()) || (root && root_option)) {
            *error = tr("Specify each storage profile option once, with a non-empty value.");
            return nullptr;
        }
        if (profile)
            selection = value;
        else {
            root_option = true;
            requested_root = value;
        }
    }
    if ((!selection.isEmpty() && selection != "daily" && selection != "debug")
        || (!selection.isEmpty() && (root_option || launch.override_root_set))
        || (root_option && launch.override_root_set)) {
        *error = tr("Choose daily, debug, or one explicit profile directory.");
        return nullptr;
    }
    const QString daily = resolvedPath(launch.daily_root);
    if (!QDir::isAbsolutePath(launch.daily_root) || daily.isEmpty()) {
        *error = tr("The daily library directory is unavailable.");
        return nullptr;
    }
    QString kind = selection.isEmpty() ? (launch.debug_build ? "debug" : "daily") : selection;
    if (root_option || launch.override_root_set) {
        kind = "custom";
        if (!root_option)
            requested_root = launch.override_root;
    } else {
        requested_root = kind == "daily" ? daily : daily + "-debug";
    }
    if (!QDir::isAbsolutePath(requested_root)) {
        *error = tr("The profile directory must be an absolute path.");
        return nullptr;
    }
    const QString root = resolvedPath(requested_root);
    if (root.isEmpty() || (kind != "daily" && rootsOverlap(daily, root))) {
        *error = tr("A separate profile must not overlap the daily library directory.");
        return nullptr;
    }
    if (kind == "custom" && rootsOverlap(daily + "-debug", root)) {
        *error = tr("Library directories must not contain one another.");
        return nullptr;
    }
    auto profile = std::unique_ptr<StorageProfile>(new StorageProfile(root, kind, launch.process));
    if (!profile->initialize(launch.legacy_brush_file, expected_id, launch.enroll_only, error))
        return nullptr;
    return profile;
}

bool StorageProfile::initialize(
    const QString& legacy_brush_file,
    const QString& expected_id,
    const bool enroll_only,
    QString* const error
) {
    if (!QDir().mkpath(root_)) {
        *error = tr("Could not create the profile directory: %1").arg(root_);
        return false;
    }
    // Validate the mutable store entrances, including settings and SQLite
    // sidecars, before opening any service. Originals remain external inputs.
    for (const auto* name :
         {"catalog.sqlite",
          "catalog.sqlite-wal",
          "catalog.sqlite-shm",
          "cache",
          "derived-rasters",
          "people",
          "location-references",
          "remote-library",
          "remote-originals",
          "library-server",
          "lut-store",
          "profiles",
          "profile",
          "ui-preferences.ini",
          "paint-brushes.ini",
          "map-provider-credentials.ini",
          "remote-library-credentials.ini",
          "storage-profile.json",
          "storage-profile.lock",
          "storage-profile-initialization.lock",
          "server-manager.ini",
          "server-manager.lock",
          "library-server-credentials.ini"}) {
        const QString path = QDir(root_).filePath(QString::fromLatin1(name));
        if (!containsPath(root_, resolvedPath(path))) {
            *error = tr("Profile storage must remain inside its directory: %1").arg(path);
            return false;
        }
    }
    lock_ = std::make_unique<QLockFile>(QDir(root_).filePath(
        process_ == StorageProfileProcess::Desktop ? "storage-profile.lock" : "server-manager.lock"
    ));
    lock_->setStaleLockTime(0);
    if (!lock_->tryLock()) {
        *error = lock_->error() == QLockFile::LockFailedError
                     ? tr("This library is already open: %1").arg(root_)
                     : tr("Could not lock the profile directory: %1").arg(root_);
        return false;
    }
    QLockFile initialization_lock(QDir(root_).filePath("storage-profile-initialization.lock"));
    initialization_lock.setStaleLockTime(0);
    if (!initialization_lock.tryLock()) {
        *error = tr("Could not initialize the profile directory: %1").arg(root_);
        return false;
    }
    const QString manifest_path = QDir(root_).filePath("storage-profile.json");
    QJsonObject manifest;
    bool changed = false;
    if (QFileInfo::exists(manifest_path)) {
        QByteArray bytes;
        if (!boundedFileBytes(manifest_path, &bytes)) {
            *error = tr("Could not read the storage profile: %1").arg(manifest_path);
            return false;
        }
        manifest = QJsonDocument::fromJson(bytes).object();
        id_ = manifest.value("id").toString();
        display_name_ = manifest.value("displayName").toString();
        if (manifest.value("schemaVersion").toInt() != 1 || QUuid(id_).isNull()
            || manifest.value("kind").toString() != kind_
            || (manifest.contains("displayName") && !manifest.value("displayName").isString())
            || (!display_name_.isEmpty() && !validDisplayName(display_name_))) {
            *error = tr("The storage profile is incompatible with this selection: %1").arg(root_);
            return false;
        }
    } else {
        if (!expected_id.isEmpty()) {
            *error = tr(
                "The library at this location has changed. Open it again to confirm its identity."
            );
            return false;
        }
        id_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
        manifest = {{"schemaVersion", 1}, {"id", id_}, {"kind", kind_}};
        changed = true;
    }
    if (!expected_id.isEmpty() && id_ != expected_id) {
        *error =
            tr("The library at this location has changed. Open it again to confirm its identity.");
        return false;
    }
    const QString migration_key = process_ == StorageProfileProcess::Desktop
                                      ? QStringLiteral("desktopSettingsPreserved")
                                      : QStringLiteral("serverSettingsPreserved");
    if (!enroll_only && isDaily() && !manifest.value(migration_key).toBool()) {
        if (!migrateDailySettings(legacy_brush_file, error))
            return false;
        manifest.insert(migration_key, true);
        changed = true;
    }
    if (changed && !writeAtomically(manifest_path, QJsonDocument(manifest).toJson())) {
        *error = tr("Could not save the storage profile: %1").arg(manifest_path);
        return false;
    }
    return true;
}

bool StorageProfile::migrateDailySettings(const QString& legacy_brush_file, QString* const error) {
    QTemporaryFile staging(QDir(root_).filePath(".profile-settings-XXXXXX"));
    if (!staging.open()) {
        *error = tr("Could not preserve the daily library settings.");
        return false;
    }
    staging.close();
    {
        auto legacy = process_ == StorageProfileProcess::Desktop ? std::make_unique<QSettings>()
                                                                 : std::make_unique<QSettings>(
                                                                       QSettings::NativeFormat,
                                                                       QSettings::UserScope,
                                                                       QStringLiteral("Shadow"),
                                                                       QStringLiteral("Shadow")
                                                                   );
        legacy->setFallbacksEnabled(false);
        QSettings current(settingsFile(), QSettings::IniFormat);
        current.setFallbacksEnabled(false);
        QSettings destination(staging.fileName(), QSettings::IniFormat);
        destination.setFallbacksEnabled(false);
        for (const QString& key : legacy->allKeys())
            destination.setValue(key, legacy->value(key));
        // Already profile-local values always win over legacy global values.
        for (const QString& key : current.allKeys())
            destination.setValue(key, current.value(key));
        destination.sync();
        if (legacy->status() != QSettings::NoError || current.status() != QSettings::NoError
            || destination.status() != QSettings::NoError) {
            *error = tr("Could not preserve the daily library settings.");
            return false;
        }
    }
    QByteArray bytes;
    if (!boundedFileBytes(staging.fileName(), &bytes) || !writeAtomically(settingsFile(), bytes)) {
        *error = tr("Could not preserve the daily library settings.");
        return false;
    }
    if (process_ == StorageProfileProcess::Desktop && !legacy_brush_file.isEmpty()
        && QFileInfo::exists(legacy_brush_file) && !QFileInfo::exists(brushSettingsFile())) {
        if (!boundedFileBytes(legacy_brush_file, &bytes)
            || !writeAtomically(brushSettingsFile(), bytes)) {
            *error = tr("Could not preserve the daily library brush presets.");
            return false;
        }
    }
    return true;
}

QString StorageProfile::root() const {
    return root_;
}
QString StorageProfile::kind() const {
    return kind_;
}
QString StorageProfile::id() const {
    return id_;
}
bool StorageProfile::isDaily() const {
    return kind_ == "daily";
}
QString StorageProfile::catalogPath() const {
    return QDir(root_).filePath("catalog.sqlite");
}
QString StorageProfile::cacheRoot() const {
    return QDir(root_).filePath("cache");
}
QString StorageProfile::settingsFile() const {
    return QDir(root_).filePath(
        process_ == StorageProfileProcess::Desktop ? "ui-preferences.ini" : "server-manager.ini"
    );
}
QString StorageProfile::brushSettingsFile() const {
    return QDir(root_).filePath("paint-brushes.ini");
}
bool StorageProfile::bindProcessEnvironment() const {
    return qputenv("SHADOW_DESKTOP_DATA_ROOT", root_.toUtf8())
           && qputenv("SHADOW_PAINT_BRUSH_SETTINGS_FILE", brushSettingsFile().toUtf8());
}
