#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <memory>

class QGuiApplication;
class QQmlEngine;
class QSettings;
class QTranslator;

/// Persistent, platform-aware UI preferences exposed to QML.
///
/// Appearance and language store stable, non-localized identifiers. The
/// effective values are resolved separately so `system` can react to the host
/// without rewriting the user's preference.
class UiPreferences final : public QObject {
    Q_OBJECT
    Q_PROPERTY(
        QString appearanceMode
        READ appearanceMode
        WRITE setAppearanceMode
        NOTIFY appearanceModeChanged
    )
    Q_PROPERTY(
        QString effectiveAppearance
        READ effectiveAppearance
        NOTIFY effectiveAppearanceChanged
    )
    Q_PROPERTY(bool dark READ dark NOTIFY effectiveAppearanceChanged)
    Q_PROPERTY(
        QString languageMode
        READ languageMode
        WRITE setLanguageMode
        NOTIFY languageModeChanged
    )
    Q_PROPERTY(
        QString effectiveLanguage
        READ effectiveLanguage
        NOTIFY effectiveLanguageChanged
    )
    Q_PROPERTY(QStringList exifFields READ exifFields NOTIFY exifFieldsChanged)

public:
    explicit UiPreferences(
        QGuiApplication& application,
        const QString& isolated_settings_file = {},
        QObject* parent = nullptr
    );
    ~UiPreferences() override;

    UiPreferences(const UiPreferences&) = delete;
    UiPreferences& operator=(const UiPreferences&) = delete;
    UiPreferences(UiPreferences&&) = delete;
    UiPreferences& operator=(UiPreferences&&) = delete;

    [[nodiscard]] QString appearanceMode() const;
    [[nodiscard]] QString effectiveAppearance() const;
    [[nodiscard]] bool dark() const noexcept;
    [[nodiscard]] QString languageMode() const;
    [[nodiscard]] QString effectiveLanguage() const;
    [[nodiscard]] QStringList exifFields() const;
    Q_INVOKABLE bool exifFieldVisible(const QString& field) const;
    Q_INVOKABLE void setExifFieldVisible(const QString& field, bool visible);
    Q_INVOKABLE void resetExifFields();

    void setAppearanceMode(const QString& mode);
    void setLanguageMode(const QString& mode);

    /// Attaches the live QML engine after translators have been installed.
    /// Later language changes retranslate the already-created object tree.
    void attachEngine(QQmlEngine& engine) noexcept;

signals:
    void appearanceModeChanged();
    void effectiveAppearanceChanged();
    void languageModeChanged();
    void effectiveLanguageChanged();
    void exifFieldsChanged();

private:
    [[nodiscard]] static QString normalizeAppearanceMode(const QString& mode);
    [[nodiscard]] static QString normalizeLanguageMode(const QString& mode);
    [[nodiscard]] QString resolveEffectiveLanguage() const;
    [[nodiscard]] static QStringList normalizeExifFields(const QStringList& fields);
    [[nodiscard]] static QStringList defaultExifFields();

    void applyAppearance();
    void refreshEffectiveAppearance();
    void applyLanguage();
    void persist(const QString& key, const QString& value);

    QGuiApplication& application_;
    std::unique_ptr<QSettings> settings_;
    std::unique_ptr<QTranslator> translator_;
    QPointer<QQmlEngine> engine_;
    QString appearance_mode_;
    QString effective_appearance_;
    QString language_mode_;
    QString effective_language_;
    QStringList exif_fields_;
    bool appearance_environment_override_ = false;
    bool language_environment_override_ = false;
    bool translator_installed_ = false;
};
