#include "lut_library.hpp"

#include <shadow/image/lut.hpp>

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QSaveFile>
#include <QUrl>
#include <QVariantMap>

#include <algorithm>
#include <exception>

namespace {

constexpr auto directories_settings_key = "lut/directories";
constexpr qint64 maximum_cube_bytes = 16LL * 1'024LL * 1'024LL;
constexpr int maximum_lut_files = 8'192;

[[nodiscard]] QString canonical_directory(const QString& path) {
    const QFileInfo info(path);
    if (!info.exists() || !info.isDir()) {
        return {};
    }
    const QString canonical = info.canonicalFilePath();
    return QDir::cleanPath(canonical.isEmpty() ? info.absoluteFilePath() : canonical);
}

[[nodiscard]] QVariantMap inspect_cube(
    const QString& path,
    const QString& root,
    const QString& managed_store_root
) {
    const QFileInfo info(path);
    QVariantMap entry{
        {QStringLiteral("path"), info.absoluteFilePath()},
        {QStringLiteral("fileName"), info.fileName()},
        {QStringLiteral("directory"), root},
        {QStringLiteral("valid"), false},
    };
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        entry.insert(QStringLiteral("error"), LutLibrary::tr("Cannot read this file"));
        return entry;
    }
    if (file.size() <= 0 || file.size() > maximum_cube_bytes) {
        entry.insert(
            QStringLiteral("error"),
            LutLibrary::tr("File must contain 1 byte through 16 MiB")
        );
        return entry;
    }
    const QByteArray bytes = file.readAll();
    try {
        const auto lut = shadow::image::parse_cube_lut(
            std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size()))
        );
        const QString id = QString::fromLatin1(
            QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()
        );
        entry.insert(QStringLiteral("id"), id);
        entry.insert(
            QStringLiteral("title"),
            lut.title.empty() ? info.completeBaseName() : QString::fromStdString(lut.title)
        );
        entry.insert(QStringLiteral("size"), lut.size);
        QString managed_path = info.absoluteFilePath();
        if (!managed_store_root.isEmpty()) {
            if (!QDir().mkpath(managed_store_root)) {
                throw std::runtime_error("cannot create the managed LUT store");
            }
            managed_path = QDir(managed_store_root).filePath(id + QStringLiteral(".cube"));
            QFile managed(managed_path);
            if (managed.exists()) {
                if (!managed.open(QIODevice::ReadOnly) || managed.readAll() != bytes) {
                    throw std::runtime_error("managed LUT content does not match its content id");
                }
            } else {
                QSaveFile output(managed_path);
                if (!output.open(QIODevice::WriteOnly)
                    || output.write(bytes) != bytes.size() || !output.commit()) {
                    throw std::runtime_error("cannot persist the managed LUT resource");
                }
            }
        }
        entry.insert(QStringLiteral("managedPath"), managed_path);
        entry.insert(QStringLiteral("valid"), true);
    } catch (const std::exception& error) {
        entry.insert(QStringLiteral("error"), QString::fromUtf8(error.what()));
    }
    return entry;
}

} // namespace

LutLibrary::LutLibrary(
    const QString& isolated_settings_file,
    const QString& managed_store_root,
    QObject* const parent
)
    : QObject(parent),
      settings_(isolated_settings_file.isEmpty()
              ? std::make_unique<QSettings>()
              : std::make_unique<QSettings>(isolated_settings_file, QSettings::IniFormat)),
      managed_store_root_(managed_store_root.isEmpty()
              ? QString{} : QDir::cleanPath(managed_store_root)) {
    const QStringList stored = settings_->value(
        QString::fromLatin1(directories_settings_key)
    ).toStringList();
    for (const QString& path : stored) {
        const QString canonical = canonical_directory(path);
        if (!canonical.isEmpty() && !directories_.contains(canonical)) {
            directories_.append(canonical);
        }
    }
    rescan();
}

LutLibrary::~LutLibrary() = default;

QStringList LutLibrary::directories() const {
    return directories_;
}

QVariantList LutLibrary::entries() const {
    return entries_;
}

QVariantList LutLibrary::availableEntries() const {
    QVariantList available;
    available.reserve(valid_count_);
    for (const QVariant& entry : entries_) {
        if (entry.toMap().value(QStringLiteral("valid")).toBool()) {
            available.append(entry);
        }
    }
    return available;
}

int LutLibrary::validCount() const noexcept {
    return valid_count_;
}

int LutLibrary::errorCount() const noexcept {
    return error_count_;
}

bool LutLibrary::addDirectory(const QUrl& directory_url) {
    const QString canonical = canonical_directory(
        directory_url.isLocalFile() ? directory_url.toLocalFile() : directory_url.toString()
    );
    if (canonical.isEmpty()) {
        return false;
    }
    if (!directories_.contains(canonical)) {
        directories_.append(canonical);
        persistDirectories();
    }
    rescan();
    return true;
}

void LutLibrary::removeDirectory(const QString& directory) {
    const QString clean = QDir::cleanPath(directory);
    if (directories_.removeAll(clean) == 0) {
        return;
    }
    persistDirectories();
    rescan();
}

void LutLibrary::rescan() {
    QVariantList next;
    for (const QString& root : directories_) {
        QDirIterator iterator(
            root,
            QStringList{QStringLiteral("*.cube")},
            QDir::Files | QDir::Readable,
            QDirIterator::Subdirectories
        );
        while (iterator.hasNext() && next.size() < maximum_lut_files) {
            next.append(inspect_cube(iterator.next(), root, managed_store_root_));
        }
        if (next.size() >= maximum_lut_files) {
            break;
        }
    }
    std::ranges::sort(next, [](const QVariant& left, const QVariant& right) {
        const QVariantMap left_map = left.toMap();
        const QVariantMap right_map = right.toMap();
        const QString left_name = left_map.value(
            left_map.value(QStringLiteral("valid")).toBool()
                ? QStringLiteral("title") : QStringLiteral("fileName")
        ).toString();
        const QString right_name = right_map.value(
            right_map.value(QStringLiteral("valid")).toBool()
                ? QStringLiteral("title") : QStringLiteral("fileName")
        ).toString();
        return left_name.localeAwareCompare(right_name) < 0;
    });
    int valid = 0;
    for (const QVariant& value : next) {
        valid += value.toMap().value(QStringLiteral("valid")).toBool() ? 1 : 0;
    }
    entries_ = std::move(next);
    valid_count_ = valid;
    error_count_ = static_cast<int>(entries_.size()) - valid_count_;
    emit libraryChanged();
}

void LutLibrary::persistDirectories() {
    settings_->setValue(QString::fromLatin1(directories_settings_key), directories_);
    settings_->sync();
}
