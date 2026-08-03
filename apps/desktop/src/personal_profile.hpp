#pragma once

#include <QObject>
#include <QString>
#include <QUrl>

#include <memory>

class QSettings;

/// Device-local personal context used to derive private Library experiences.
///
/// This owner persists nickname, one provider-independent home locality, and
/// a normalized local avatar. It has no account, network, Catalog, or photo
/// query responsibility.
class PersonalProfile final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString nickname READ nickname WRITE setNickname NOTIFY profileChanged)
    Q_PROPERTY(QString homeLocalityKey READ homeLocalityKey NOTIFY profileChanged)
    Q_PROPERTY(QString homeLocalityLabel READ homeLocalityLabel NOTIFY profileChanged)
    Q_PROPERTY(bool homeConfigured READ homeConfigured NOTIFY profileChanged)
    Q_PROPERTY(QUrl avatarUrl READ avatarUrl NOTIFY avatarChanged)
    Q_PROPERTY(QString avatarInitial READ avatarInitial NOTIFY profileChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY errorTextChanged)

  public:
    explicit PersonalProfile(
        const QString& application_data_root,
        const QString& isolated_settings_file = {},
        QObject* parent = nullptr
    );
    ~PersonalProfile() override;

    PersonalProfile(const PersonalProfile&) = delete;
    PersonalProfile& operator=(const PersonalProfile&) = delete;

    [[nodiscard]] QString nickname() const;
    [[nodiscard]] QString homeLocalityKey() const;
    [[nodiscard]] QString homeLocalityLabel() const;
    [[nodiscard]] bool homeConfigured() const noexcept;
    [[nodiscard]] QUrl avatarUrl() const;
    [[nodiscard]] QString avatarInitial() const;
    [[nodiscard]] QString errorText() const;

    void setNickname(const QString& nickname);
    Q_INVOKABLE void setHomeLocality(const QString& key, const QString& label);
    Q_INVOKABLE void clearHomeLocality();
    Q_INVOKABLE bool importAvatar(const QUrl& source_url);
    Q_INVOKABLE void clearAvatar();

  signals:
    void profileChanged();
    void avatarChanged();
    void errorTextChanged();

  private:
    [[nodiscard]] static QString normalizeNickname(const QString& nickname);
    [[nodiscard]] static QString normalizeHomeKey(const QString& key);
    [[nodiscard]] static QString normalizeHomeLabel(const QString& label);
    void setErrorText(QString error);
    void persist();

    std::unique_ptr<QSettings> settings_;
    QString avatar_path_;
    QString nickname_;
    QString home_locality_key_;
    QString home_locality_label_;
    QString error_text_;
};
