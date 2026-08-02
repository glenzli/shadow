#include "ai_preferences.hpp"

#include <QDir>
#include <QSettings>
#include <QVariant>

#include <algorithm>

namespace {

constexpr auto raw_denoise_allowed_key = "ai/raw_denoise_execution_allowed";
constexpr auto subject_mask_allowed_key = "ai/subject_mask_execution_allowed";
constexpr auto raw_denoise_default_amount_key = "ai/raw_denoise_default_amount";

} // namespace

AiPreferences::AiPreferences(
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
    model_storage_path_(
        QDir::cleanPath(QDir(application_data_root).filePath(QStringLiteral("models")))
    ) {
    QDir().mkpath(model_storage_path_);
    raw_denoise_execution_allowed_ =
        settings_
            ->value(QString::fromLatin1(raw_denoise_allowed_key), raw_denoise_execution_allowed_)
            .toBool();
    subject_mask_execution_allowed_ =
        settings_
            ->value(QString::fromLatin1(subject_mask_allowed_key), subject_mask_execution_allowed_)
            .toBool();
    raw_denoise_default_amount_ = std::clamp(
        settings_
            ->value(
                QString::fromLatin1(raw_denoise_default_amount_key),
                raw_denoise_default_amount_
            )
            .toInt(),
        0,
        100
    );
}

AiPreferences::~AiPreferences() = default;

bool AiPreferences::rawDenoiseExecutionAllowed() const noexcept {
    return raw_denoise_execution_allowed_;
}

bool AiPreferences::subjectMaskExecutionAllowed() const noexcept {
    return subject_mask_execution_allowed_;
}

int AiPreferences::rawDenoiseDefaultAmount() const noexcept {
    return raw_denoise_default_amount_;
}

QString AiPreferences::modelStoragePath() const {
    return model_storage_path_;
}

QUrl AiPreferences::modelStorageUrl() const {
    return QUrl::fromLocalFile(model_storage_path_);
}

void AiPreferences::setRawDenoiseExecutionAllowed(const bool allowed) {
    if (raw_denoise_execution_allowed_ == allowed) {
        return;
    }
    raw_denoise_execution_allowed_ = allowed;
    persist(raw_denoise_allowed_key, allowed);
    emit rawDenoiseExecutionAllowedChanged();
}

void AiPreferences::setSubjectMaskExecutionAllowed(const bool allowed) {
    if (subject_mask_execution_allowed_ == allowed) {
        return;
    }
    subject_mask_execution_allowed_ = allowed;
    persist(subject_mask_allowed_key, allowed);
    emit subjectMaskExecutionAllowedChanged();
}

void AiPreferences::setRawDenoiseDefaultAmount(const int amount_percent) {
    const int normalized = std::clamp(amount_percent, 0, 100);
    if (raw_denoise_default_amount_ == normalized) {
        return;
    }
    raw_denoise_default_amount_ = normalized;
    persist(raw_denoise_default_amount_key, normalized);
    emit rawDenoiseDefaultAmountChanged();
}

void AiPreferences::persist(const char* const key, const QVariant& value) {
    settings_->setValue(QString::fromLatin1(key), value);
    settings_->sync();
}
