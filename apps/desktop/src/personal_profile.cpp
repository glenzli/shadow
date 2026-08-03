#include "personal_profile.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QUuid>

#include <algorithm>
#include <utility>

namespace {

constexpr auto nickname_key = "profile/nickname";
constexpr auto living_places_key = "profile/living_places_v1";
constexpr auto home_locality_key = "profile/home_locality_key";
constexpr auto home_locality_label_key = "profile/home_locality_label";
constexpr int avatar_edge = 512;
constexpr int maximum_living_places = 32;

[[nodiscard]] QVariantList decode_living_places(const QVariant& stored) {
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(stored.toByteArray(), &error);
    if (error.error != QJsonParseError::NoError || !document.isArray()) {
        return {};
    }
    return document.array().toVariantList();
}

} // namespace

PersonalProfile::PersonalProfile(
    const QString& application_data_root,
    const QString& isolated_settings_file,
    QObject* const parent
) :
    QObject(parent),
    settings_(
        isolated_settings_file.isEmpty()
            ? std::make_unique<QSettings>()
            : std::make_unique<QSettings>(isolated_settings_file, QSettings::IniFormat)
    ),
    avatar_path_(QDir(application_data_root).filePath(QStringLiteral("profile/avatar.png"))),
    nickname_(normalizeNickname(settings_->value(QString::fromLatin1(nickname_key)).toString())) {
    bool valid = false;
    living_places_ = normalizeLivingPlaces(
        decode_living_places(settings_->value(QString::fromLatin1(living_places_key))),
        &valid
    );
    if (!valid) {
        living_places_.clear();
    }
    // The pre-release single-home keys are intentionally not migrated. The
    // living-place timeline is the only persisted v1 contract.
    settings_->remove(QString::fromLatin1(home_locality_key));
    settings_->remove(QString::fromLatin1(home_locality_label_key));
    settings_->sync();
}

PersonalProfile::~PersonalProfile() = default;

QString PersonalProfile::nickname() const {
    return nickname_;
}

QVariantList PersonalProfile::livingPlaces() const {
    return living_places_;
}

bool PersonalProfile::hasLivingPlaces() const noexcept {
    return !living_places_.isEmpty();
}

QUrl PersonalProfile::avatarUrl() const {
    const QFileInfo avatar(avatar_path_);
    if (!avatar.isFile()) {
        return {};
    }
    QUrl url = QUrl::fromLocalFile(avatar.absoluteFilePath());
    url.setQuery(QStringLiteral("v=%1").arg(avatar.lastModified().toMSecsSinceEpoch()));
    return url;
}

QString PersonalProfile::avatarInitial() const {
    const QString trimmed = nickname_.trimmed();
    return trimmed.isEmpty() ? QStringLiteral("S") : trimmed.left(1).toUpper();
}

QString PersonalProfile::errorText() const {
    return error_text_;
}

void PersonalProfile::setNickname(const QString& nickname) {
    const QString normalized = normalizeNickname(nickname);
    if (nickname_ == normalized) {
        return;
    }
    nickname_ = normalized;
    persist();
    emit profileChanged();
}

bool PersonalProfile::replaceLivingPlaces(const QVariantList& living_places) {
    bool valid = false;
    const QVariantList normalized = normalizeLivingPlaces(living_places, &valid);
    if (!valid) {
        setErrorText(tr("Check that every living place has a city and valid YYYY-MM dates."));
        return false;
    }
    setErrorText({});
    if (living_places_ == normalized) {
        return true;
    }
    living_places_ = normalized;
    persist();
    emit profileChanged();
    return true;
}

bool PersonalProfile::importAvatar(const QUrl& source_url) {
    const QString source_path = source_url.toLocalFile();
    QImageReader reader(source_path);
    reader.setAutoTransform(true);
    QImage image = reader.read();
    if (source_path.isEmpty() || image.isNull()) {
        setErrorText(tr("The selected avatar could not be read as an image."));
        return false;
    }

    const QImage scaled = image.scaled(
        avatar_edge,
        avatar_edge,
        Qt::KeepAspectRatioByExpanding,
        Qt::SmoothTransformation
    );
    const int left = std::max(0, (scaled.width() - avatar_edge) / 2);
    const int top = std::max(0, (scaled.height() - avatar_edge) / 2);
    const QImage square = scaled.copy(left, top, avatar_edge, avatar_edge);
    const QFileInfo destination(avatar_path_);
    if (!QDir().mkpath(destination.absolutePath()) || !square.save(avatar_path_, "PNG")) {
        setErrorText(tr("The avatar could not be saved to Shadow's local profile."));
        return false;
    }
    setErrorText({});
    emit avatarChanged();
    return true;
}

void PersonalProfile::clearAvatar() {
    setErrorText({});
    if (!QFileInfo::exists(avatar_path_)) {
        return;
    }
    if (!QFile::remove(avatar_path_)) {
        setErrorText(tr("The local avatar could not be removed."));
        return;
    }
    emit avatarChanged();
}

QString PersonalProfile::normalizeNickname(const QString& nickname) {
    return nickname.trimmed().left(80);
}

QVariantList
PersonalProfile::normalizeLivingPlaces(const QVariantList& living_places, bool* const valid) {
    *valid = false;
    if (living_places.size() > maximum_living_places) {
        return {};
    }
    static const QRegularExpression month_pattern(QStringLiteral("^[0-9]{4}-(0[1-9]|1[0-2])$"));
    QVariantList normalized;
    normalized.reserve(living_places.size());
    QSet<QString> identities;
    for (const QVariant& value : living_places) {
        const QVariantMap source = value.toMap();
        const QString key = source.value(QStringLiteral("key")).toString().trimmed().toLower();
        const QString label = source.value(QStringLiteral("label")).toString().trimmed();
        const QString start = source.value(QStringLiteral("startMonth")).toString().trimmed();
        const QString end = source.value(QStringLiteral("endMonth")).toString().trimmed();
        if (key.isEmpty() || key.size() > 512 || label.isEmpty() || label.size() > 256
            || (!start.isEmpty() && !month_pattern.match(start).hasMatch())
            || (!end.isEmpty() && !month_pattern.match(end).hasMatch())
            || (!start.isEmpty() && !end.isEmpty() && start > end)) {
            return {};
        }
        const QString identity = key + QChar::Null + start + QChar::Null + end;
        if (identities.contains(identity)) {
            continue;
        }
        identities.insert(identity);
        QString id = source.value(QStringLiteral("id")).toString().trimmed().left(80);
        if (id.isEmpty()) {
            id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        }
        normalized.push_back(
            QVariantMap{
                {QStringLiteral("id"), id},
                {QStringLiteral("key"), key},
                {QStringLiteral("label"), label},
                {QStringLiteral("startMonth"), start},
                {QStringLiteral("endMonth"), end},
            }
        );
    }
    *valid = true;
    return normalized;
}

void PersonalProfile::setErrorText(QString error) {
    if (error_text_ == error) {
        return;
    }
    error_text_ = std::move(error);
    emit errorTextChanged();
}

void PersonalProfile::persist() {
    settings_->setValue(QString::fromLatin1(nickname_key), nickname_);
    settings_->setValue(
        QString::fromLatin1(living_places_key),
        QJsonDocument::fromVariant(living_places_).toJson(QJsonDocument::Compact)
    );
    settings_->remove(QString::fromLatin1(home_locality_key));
    settings_->remove(QString::fromLatin1(home_locality_label_key));
    settings_->sync();
}
