#include "optics_profile_library.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSaveFile>
#include <QUuid>
#include <QVariantMap>

#include <algorithm>
#include <array>

namespace {

constexpr auto optics_profile_suffix = ".shadow-optics.json";
constexpr int optics_profile_schema_version = 1;
constexpr qint64 maximum_profile_bytes = 16LL * 1'024LL;
constexpr int manual_optics_minimum = -100;
constexpr int manual_optics_maximum = 100;
constexpr int manual_vignetting_midpoint_default = 50;
constexpr int manual_vignetting_midpoint_minimum = 0;
constexpr int manual_vignetting_midpoint_maximum = 100;
constexpr int maximum_profiles = 2'048;
constexpr int maximum_profile_text_bytes = 256;

[[nodiscard]] QString trimmed_profile_text(const QString& value) {
    return value.trimmed();
}

[[nodiscard]] bool valid_profile_text(const QString& value) {
    return !value.isEmpty() && value.toUtf8().size() <= maximum_profile_text_bytes
        && !value.contains(QChar::Null);
}

[[nodiscard]] bool profile_file_name(const QString& name) {
    return name.endsWith(QString::fromLatin1(optics_profile_suffix), Qt::CaseInsensitive);
}

[[nodiscard]] bool bounded_int(
    const QVariantMap& map,
    const QString& key,
    const int minimum,
    const int maximum,
    const int fallback,
    int* const output
) {
    if (!map.contains(key)) {
        *output = fallback;
        return true;
    }
    bool converted = false;
    const int value = map.value(key).toInt(&converted);
    if (!converted || value < minimum || value > maximum) {
        return false;
    }
    *output = value;
    return true;
}

[[nodiscard]] bool profile_corrections(
    const QVariantMap& source,
    QVariantMap* const normalized
) {
    int distortion = 0;
    int tca_red_cyan = 0;
    int tca_blue_yellow = 0;
    int vignetting_amount = 0;
    int vignetting_midpoint = manual_vignetting_midpoint_default;
    const bool valid = bounded_int(
                           source,
                           QStringLiteral("manualDistortion"),
                           manual_optics_minimum,
                           manual_optics_maximum,
                           0,
                           &distortion
                       )
        && bounded_int(
            source,
            QStringLiteral("manualTcaRedCyan"),
            manual_optics_minimum,
            manual_optics_maximum,
            0,
            &tca_red_cyan
        )
        && bounded_int(
            source,
            QStringLiteral("manualTcaBlueYellow"),
            manual_optics_minimum,
            manual_optics_maximum,
            0,
            &tca_blue_yellow
        )
        && bounded_int(
            source,
            QStringLiteral("manualVignettingAmount"),
            manual_optics_minimum,
            manual_optics_maximum,
            0,
            &vignetting_amount
        )
        && bounded_int(
            source,
            QStringLiteral("manualVignettingMidpoint"),
            manual_vignetting_midpoint_minimum,
            manual_vignetting_midpoint_maximum,
            manual_vignetting_midpoint_default,
            &vignetting_midpoint
        );
    if (!valid) {
        return false;
    }
    *normalized = {
        {QStringLiteral("manualDistortion"), distortion},
        {QStringLiteral("manualTcaRedCyan"), tca_red_cyan},
        {QStringLiteral("manualTcaBlueYellow"), tca_blue_yellow},
        {QStringLiteral("manualVignettingAmount"), vignetting_amount},
        {QStringLiteral("manualVignettingMidpoint"), vignetting_midpoint},
    };
    return true;
}

[[nodiscard]] QVariantMap entry_from_document(
    const QJsonDocument& document,
    const QFileInfo& info,
    QString* const error
) {
    if (!document.isObject()) {
        *error = OpticsProfileLibrary::tr("Profile file must contain a JSON object");
        return {};
    }
    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("schemaVersion")).toInt() != optics_profile_schema_version) {
        *error = OpticsProfileLibrary::tr("Unsupported optical profile format");
        return {};
    }
    const QString id = trimmed_profile_text(root.value(QStringLiteral("id")).toString());
    const QString title = trimmed_profile_text(root.value(QStringLiteral("title")).toString());
    const QString lens_label = trimmed_profile_text(root.value(QStringLiteral("lensLabel")).toString());
    if (QUuid(id).isNull() || !valid_profile_text(title)
        || (!lens_label.isEmpty() && lens_label.toUtf8().size() > maximum_profile_text_bytes)) {
        *error = OpticsProfileLibrary::tr("Profile identity or name is invalid");
        return {};
    }
    QVariantMap corrections;
    if (!profile_corrections(root.value(QStringLiteral("corrections")).toObject().toVariantMap(),
                             &corrections)) {
        *error = OpticsProfileLibrary::tr("Profile corrections are outside the supported range");
        return {};
    }
    QVariantMap entry{
        {QStringLiteral("id"), id},
        {QStringLiteral("title"), title},
        {QStringLiteral("lensLabel"), lens_label},
        {QStringLiteral("updatedAt"), root.value(QStringLiteral("updatedAt")).toString()},
        {QStringLiteral("path"), info.absoluteFilePath()},
    };
    for (auto iterator = corrections.cbegin(); iterator != corrections.cend(); ++iterator) {
        entry.insert(iterator.key(), iterator.value());
    }
    return entry;
}

[[nodiscard]] QJsonObject json_corrections(const QVariantMap& corrections) {
    return QJsonObject{
        {QStringLiteral("manualDistortion"), corrections.value(QStringLiteral("manualDistortion")).toInt()},
        {QStringLiteral("manualTcaRedCyan"), corrections.value(QStringLiteral("manualTcaRedCyan")).toInt()},
        {QStringLiteral("manualTcaBlueYellow"), corrections.value(QStringLiteral("manualTcaBlueYellow")).toInt()},
        {QStringLiteral("manualVignettingAmount"), corrections.value(QStringLiteral("manualVignettingAmount")).toInt()},
        {QStringLiteral("manualVignettingMidpoint"), corrections.value(QStringLiteral("manualVignettingMidpoint")).toInt()},
    };
}

} // namespace

OpticsProfileLibrary::OpticsProfileLibrary(QString root_directory, QObject* const parent)
    : QObject(parent),
      root_directory_(QDir::cleanPath(std::move(root_directory))) {
    rescan();
}

QString OpticsProfileLibrary::rootDirectory() const {
    return root_directory_;
}

QVariantList OpticsProfileLibrary::entries() const {
    return entries_;
}

int OpticsProfileLibrary::count() const noexcept {
    return static_cast<int>(entries_.size());
}

QString OpticsProfileLibrary::lastError() const {
    return last_error_;
}

bool OpticsProfileLibrary::saveProfile(
    const QString& title,
    const QString& lens_label,
    const QVariantMap& corrections
) {
    const QString normalized_title = trimmed_profile_text(title);
    const QString normalized_lens_label = trimmed_profile_text(lens_label);
    QVariantMap normalized_corrections;
    if (!valid_profile_text(normalized_title)) {
        last_error_ = tr("Give this optical profile a short name");
        emit libraryChanged();
        return false;
    }
    if (normalized_lens_label.toUtf8().size() > maximum_profile_text_bytes
        || !profile_corrections(corrections, &normalized_corrections)) {
        last_error_ = tr("The optical profile contains an invalid correction value");
        emit libraryChanged();
        return false;
    }
    if (!QDir().mkpath(root_directory_)) {
        last_error_ = tr("Could not create the local optical profile directory");
        emit libraryChanged();
        return false;
    }
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    const QJsonObject document{
        {QStringLiteral("schemaVersion"), optics_profile_schema_version},
        {QStringLiteral("id"), id},
        {QStringLiteral("title"), normalized_title},
        {QStringLiteral("lensLabel"), normalized_lens_label},
        {QStringLiteral("createdAt"), now},
        {QStringLiteral("updatedAt"), now},
        {QStringLiteral("corrections"), json_corrections(normalized_corrections)},
    };
    const QString file_path = QDir(root_directory_).filePath(
        id + QString::fromLatin1(optics_profile_suffix)
    );
    QSaveFile file(file_path);
    const QByteArray contents = QJsonDocument(document).toJson(QJsonDocument::Indented);
    if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size()
        || !file.commit()) {
        last_error_ = tr("Could not save the local optical profile");
        emit libraryChanged();
        return false;
    }
    rescan();
    return true;
}

bool OpticsProfileLibrary::removeProfile(const QString& profile_id) {
    const QUuid id(profile_id);
    if (id.isNull()) {
        last_error_ = tr("The selected optical profile is invalid");
        emit libraryChanged();
        return false;
    }
    const QString file_path = QDir(root_directory_).filePath(
        id.toString(QUuid::WithoutBraces) + QString::fromLatin1(optics_profile_suffix)
    );
    if (!QFileInfo::exists(file_path) || !QFile::remove(file_path)) {
        last_error_ = tr("Could not remove the selected optical profile");
        emit libraryChanged();
        return false;
    }
    rescan();
    return true;
}

void OpticsProfileLibrary::rescan() {
    QVariantList next;
    last_error_.clear();
    if (!QDir().mkpath(root_directory_)) {
        last_error_ = tr("Could not create the local optical profile directory");
        entries_.clear();
        emit libraryChanged();
        return;
    }
    const QDir directory(root_directory_);
    const QFileInfoList files = directory.entryInfoList(
        QStringList{QStringLiteral("*") + QString::fromLatin1(optics_profile_suffix)},
        QDir::Files | QDir::Readable,
        QDir::Name | QDir::IgnoreCase
    );
    for (const QFileInfo& info : files) {
        if (next.size() >= maximum_profiles || !profile_file_name(info.fileName())) {
            break;
        }
        QFile file(info.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly) || file.size() <= 0
            || file.size() > maximum_profile_bytes) {
            last_error_ = tr("One local optical profile could not be read");
            continue;
        }
        QJsonParseError parse_error;
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parse_error);
        QString entry_error;
        const QVariantMap entry = entry_from_document(document, info, &entry_error);
        if (entry.isEmpty()) {
            last_error_ = entry_error;
            continue;
        }
        next.append(entry);
    }
    std::ranges::sort(next, [](const QVariant& left, const QVariant& right) {
        return left.toMap().value(QStringLiteral("title")).toString().localeAwareCompare(
            right.toMap().value(QStringLiteral("title")).toString()
        ) < 0;
    });
    entries_ = std::move(next);
    emit libraryChanged();
}
