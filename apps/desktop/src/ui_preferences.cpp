#include "ui_preferences.hpp"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QLocale>
#include <QPalette>
#include <QQmlEngine>
#include <QSettings>
#include <QStyleHints>
#include <QTranslator>

#include <algorithm>
#include <utility>

namespace {

constexpr auto appearance_settings_key = "ui/appearance";
constexpr auto language_settings_key = "ui/language";
constexpr auto exif_fields_settings_key = "ui/exif_fields";
constexpr auto library_thumbnail_scale_settings_key = "library/thumbnail_scale";
constexpr auto appearance_environment_key = "SHADOW_DESKTOP_APPEARANCE_MODE";
constexpr auto language_environment_key = "SHADOW_DESKTOP_LANGUAGE_MODE";

[[nodiscard]] QString environmentOverride(const char* const key) {
    return QString::fromUtf8(qgetenv(key)).trimmed();
}

} // namespace

UiPreferences::UiPreferences(
    QGuiApplication& application,
    const QString& isolated_settings_file,
    QObject* const parent
)
    : QObject(parent),
      application_(application),
      settings_(isolated_settings_file.isEmpty()
              ? std::make_unique<QSettings>()
              : std::make_unique<QSettings>(
                    isolated_settings_file,
                    QSettings::IniFormat
                )),
      translator_(std::make_unique<QTranslator>()) {
    const QString appearance_override = environmentOverride(
        appearance_environment_key
    );
    appearance_environment_override_ = !appearance_override.isEmpty();
    appearance_mode_ = normalizeAppearanceMode(
        appearance_environment_override_
            ? appearance_override
            : settings_->value(
                  QString::fromLatin1(appearance_settings_key),
                  QStringLiteral("system")
              ).toString()
    );

    const QString language_override = environmentOverride(language_environment_key);
    language_environment_override_ = !language_override.isEmpty();
    language_mode_ = normalizeLanguageMode(
        language_environment_override_
            ? language_override
            : settings_->value(
                  QString::fromLatin1(language_settings_key),
                  QStringLiteral("system")
              ).toString()
    );
    exif_fields_ = normalizeExifFields(
        settings_->value(
            QString::fromLatin1(exif_fields_settings_key),
            defaultExifFields()
        ).toStringList()
    );
    library_thumbnail_scale_ = normalizeLibraryThumbnailScale(
        settings_->value(
            QString::fromLatin1(library_thumbnail_scale_settings_key),
            library_thumbnail_scale_
        ).toInt()
    );

    QObject::connect(
        application_.styleHints(),
        &QStyleHints::colorSchemeChanged,
        this,
        [this](const Qt::ColorScheme) { refreshEffectiveAppearance(); }
    );
    applyAppearance();
    applyLanguage();
}

UiPreferences::~UiPreferences() {
    if (translator_installed_) {
        QCoreApplication::removeTranslator(translator_.get());
    }
}

QString UiPreferences::appearanceMode() const {
    return appearance_mode_;
}

QString UiPreferences::effectiveAppearance() const {
    return effective_appearance_;
}

bool UiPreferences::dark() const noexcept {
    return effective_appearance_ == QStringLiteral("dark");
}

QString UiPreferences::languageMode() const {
    return language_mode_;
}

QString UiPreferences::effectiveLanguage() const {
    return effective_language_;
}

QStringList UiPreferences::exifFields() const {
    return exif_fields_;
}

int UiPreferences::libraryThumbnailScale() const noexcept {
    return library_thumbnail_scale_;
}

bool UiPreferences::exifFieldVisible(const QString& field) const {
    return exif_fields_.contains(field);
}

void UiPreferences::setExifFieldVisible(const QString& field, const bool visible) {
    QStringList next = exif_fields_;
    if (visible && !next.contains(field)) {
        next.append(field);
    } else if (!visible) {
        next.removeAll(field);
    }
    next = normalizeExifFields(next);
    if (next == exif_fields_) {
        return;
    }
    exif_fields_ = std::move(next);
    settings_->setValue(QString::fromLatin1(exif_fields_settings_key), exif_fields_);
    settings_->sync();
    emit exifFieldsChanged();
}

void UiPreferences::resetExifFields() {
    const QStringList defaults = defaultExifFields();
    if (exif_fields_ == defaults) {
        return;
    }
    exif_fields_ = defaults;
    settings_->setValue(QString::fromLatin1(exif_fields_settings_key), exif_fields_);
    settings_->sync();
    emit exifFieldsChanged();
}

void UiPreferences::setAppearanceMode(const QString& mode) {
    const QString normalized = normalizeAppearanceMode(mode);
    if (appearance_mode_ == normalized) {
        return;
    }
    appearance_mode_ = normalized;
    if (!appearance_environment_override_) {
        persist(QString::fromLatin1(appearance_settings_key), appearance_mode_);
    }
    emit appearanceModeChanged();
    applyAppearance();
}

void UiPreferences::setLanguageMode(const QString& mode) {
    const QString normalized = normalizeLanguageMode(mode);
    if (language_mode_ == normalized) {
        return;
    }
    language_mode_ = normalized;
    if (!language_environment_override_) {
        persist(QString::fromLatin1(language_settings_key), language_mode_);
    }
    emit languageModeChanged();
    applyLanguage();
}

void UiPreferences::setLibraryThumbnailScale(const int scale) {
    const int normalized = normalizeLibraryThumbnailScale(scale);
    if (library_thumbnail_scale_ == normalized) {
        return;
    }
    library_thumbnail_scale_ = normalized;
    settings_->setValue(
        QString::fromLatin1(library_thumbnail_scale_settings_key),
        library_thumbnail_scale_
    );
    settings_->sync();
    emit libraryThumbnailScaleChanged();
}

void UiPreferences::attachEngine(QQmlEngine& engine) noexcept {
    engine_ = &engine;
}

QString UiPreferences::normalizeAppearanceMode(const QString& mode) {
    const QString normalized = mode.trimmed().toLower();
    if (normalized == QStringLiteral("light")
        || normalized == QStringLiteral("dark")) {
        return normalized;
    }
    return QStringLiteral("system");
}

QString UiPreferences::normalizeLanguageMode(const QString& mode) {
    const QString normalized = mode.trimmed().replace(QLatin1Char('-'), QLatin1Char('_'));
    if (normalized.compare(QStringLiteral("zh"), Qt::CaseInsensitive) == 0
        || normalized.compare(QStringLiteral("zh_cn"), Qt::CaseInsensitive) == 0
        || normalized.startsWith(QStringLiteral("zh_Hans"), Qt::CaseInsensitive)) {
        return QStringLiteral("zh_CN");
    }
    if (normalized.compare(QStringLiteral("en"), Qt::CaseInsensitive) == 0
        || normalized.startsWith(QStringLiteral("en_"), Qt::CaseInsensitive)) {
        return QStringLiteral("en");
    }
    return QStringLiteral("system");
}

QStringList UiPreferences::defaultExifFields() {
    return {
        QStringLiteral("captured_at"),
        QStringLiteral("camera"),
        QStringLiteral("lens"),
        QStringLiteral("exposure"),
        QStringLiteral("aperture"),
        QStringLiteral("iso"),
        QStringLiteral("focal_length"),
        QStringLiteral("dimensions"),
    };
}

QStringList UiPreferences::normalizeExifFields(const QStringList& fields) {
    static const QStringList allowed{
        QStringLiteral("captured_at"), QStringLiteral("camera"),
        QStringLiteral("lens"), QStringLiteral("exposure"),
        QStringLiteral("aperture"), QStringLiteral("iso"),
        QStringLiteral("focal_length"), QStringLiteral("dimensions"),
        QStringLiteral("focal_length_35mm"), QStringLiteral("raw_dimensions"),
        QStringLiteral("sensor_bits"), QStringLiteral("cfa"),
        QStringLiteral("dng"),
    };
    QStringList normalized;
    for (const QString& allowed_field : allowed) {
        if (fields.contains(allowed_field)) {
            normalized.append(allowed_field);
        }
    }
    return normalized;
}

int UiPreferences::normalizeLibraryThumbnailScale(const int scale) noexcept {
    return std::clamp(scale, 96, 360);
}

QString UiPreferences::resolveEffectiveLanguage() const {
    if (language_mode_ != QStringLiteral("system")) {
        return language_mode_;
    }
    for (const QString& language : QLocale::system().uiLanguages()) {
        const QString normalized = language.trimmed().replace(
            QLatin1Char('-'),
            QLatin1Char('_')
        );
        if (normalized.compare(QStringLiteral("zh"), Qt::CaseInsensitive) == 0
            || normalized.startsWith(QStringLiteral("zh_"), Qt::CaseInsensitive)) {
            return QStringLiteral("zh_CN");
        }
    }
    return QStringLiteral("en");
}

void UiPreferences::applyAppearance() {
    if (appearance_mode_ == QStringLiteral("light")) {
        application_.styleHints()->setColorScheme(Qt::ColorScheme::Light);
    } else if (appearance_mode_ == QStringLiteral("dark")) {
        application_.styleHints()->setColorScheme(Qt::ColorScheme::Dark);
    } else {
        application_.styleHints()->unsetColorScheme();
    }
    refreshEffectiveAppearance();
}

void UiPreferences::refreshEffectiveAppearance() {
    bool is_dark = appearance_mode_ == QStringLiteral("dark");
    if (appearance_mode_ == QStringLiteral("light")) {
        is_dark = false;
    } else if (appearance_mode_ == QStringLiteral("system")) {
        is_dark = application_.styleHints()->colorScheme() == Qt::ColorScheme::Dark;
    }
    if (appearance_mode_ == QStringLiteral("system")
        && application_.styleHints()->colorScheme() == Qt::ColorScheme::Unknown) {
        is_dark = application_.palette().color(QPalette::Window).lightnessF() < 0.5;
    }
    const QString effective = is_dark ? QStringLiteral("dark") : QStringLiteral("light");
    if (effective_appearance_ == effective) {
        return;
    }
    effective_appearance_ = effective;
    emit effectiveAppearanceChanged();
}

void UiPreferences::applyLanguage() {
    QString effective = resolveEffectiveLanguage();
    auto next_translator = std::make_unique<QTranslator>();
    bool install_next = false;
    if (effective == QStringLiteral("zh_CN")) {
        install_next = next_translator->load(
            QStringLiteral(":/i18n/shadow_zh_CN.qm")
        );
        if (!install_next) {
            effective = QStringLiteral("en");
        }
    }

    if (translator_installed_) {
        QCoreApplication::removeTranslator(translator_.get());
    }
    translator_ = std::move(next_translator);
    translator_installed_ = install_next;
    if (translator_installed_) {
        QCoreApplication::installTranslator(translator_.get());
    }

    // Formatting is part of the selected UI language too. Leaving Qt's
    // default locale on the host setting can otherwise produce an English
    // interface with Chinese dates (or vice versa).
    QLocale::setDefault(
        QLocale(effective == QStringLiteral("zh_CN")
                ? QStringLiteral("zh_CN")
                : QStringLiteral("en_US"))
    );

    const bool changed = effective_language_ != effective;
    effective_language_ = std::move(effective);
    if (engine_ != nullptr) {
        engine_->retranslate();
    }
    if (changed) {
        emit effectiveLanguageChanged();
    }
}

void UiPreferences::persist(const QString& key, const QString& value) {
    settings_->setValue(key, value);
    settings_->sync();
}
