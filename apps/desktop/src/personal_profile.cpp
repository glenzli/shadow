#include "personal_profile.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QSettings>

#include <algorithm>
#include <utility>

namespace {

constexpr auto nickname_key = "profile/nickname";
constexpr auto home_locality_key = "profile/home_locality_key";
constexpr auto home_locality_label_key = "profile/home_locality_label";
constexpr int avatar_edge = 512;

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
    nickname_(normalizeNickname(settings_->value(QString::fromLatin1(nickname_key)).toString())),
    home_locality_key_(
        normalizeHomeKey(settings_->value(QString::fromLatin1(home_locality_key)).toString())
    ),
    home_locality_label_(normalizeHomeLabel(
        settings_->value(QString::fromLatin1(home_locality_label_key)).toString()
    )) {
    if (home_locality_key_.isEmpty()) {
        home_locality_label_.clear();
    }
}

PersonalProfile::~PersonalProfile() = default;

QString PersonalProfile::nickname() const {
    return nickname_;
}

QString PersonalProfile::homeLocalityKey() const {
    return home_locality_key_;
}

QString PersonalProfile::homeLocalityLabel() const {
    return home_locality_label_;
}

bool PersonalProfile::homeConfigured() const noexcept {
    return !home_locality_key_.isEmpty();
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

void PersonalProfile::setHomeLocality(const QString& key, const QString& label) {
    const QString normalized_key = normalizeHomeKey(key);
    const QString normalized_label = normalizeHomeLabel(label);
    if (normalized_key.isEmpty() || normalized_label.isEmpty()) {
        setErrorText(tr("Choose a valid home location from your Library places."));
        return;
    }
    setErrorText({});
    if (home_locality_key_ == normalized_key && home_locality_label_ == normalized_label) {
        return;
    }
    home_locality_key_ = normalized_key;
    home_locality_label_ = normalized_label;
    persist();
    emit profileChanged();
}

void PersonalProfile::clearHomeLocality() {
    setErrorText({});
    if (home_locality_key_.isEmpty() && home_locality_label_.isEmpty()) {
        return;
    }
    home_locality_key_.clear();
    home_locality_label_.clear();
    persist();
    emit profileChanged();
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

QString PersonalProfile::normalizeHomeKey(const QString& key) {
    return key.trimmed().toLower().left(512);
}

QString PersonalProfile::normalizeHomeLabel(const QString& label) {
    return label.trimmed().left(256);
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
    settings_->setValue(QString::fromLatin1(home_locality_key), home_locality_key_);
    settings_->setValue(QString::fromLatin1(home_locality_label_key), home_locality_label_);
    settings_->sync();
}
