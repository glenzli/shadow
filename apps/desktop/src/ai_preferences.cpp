#include "ai_preferences.hpp"

#include <QDir>
#include <QSettings>
#include <QVariant>

#include <algorithm>

namespace {

constexpr auto raw_denoise_allowed_key = "ai/raw_denoise_execution_allowed";
constexpr auto subject_mask_allowed_key = "ai/subject_mask_execution_allowed";
constexpr auto image_completion_allowed_key = "ai/image_completion_execution_allowed";
constexpr auto people_analysis_consent_state_key = "ai/people_analysis_consent_state";
constexpr auto people_analysis_consent_revision_key = "ai/people_analysis_consent_revision";
constexpr int people_analysis_consent_revision = 1;
constexpr auto image_understanding_allowed_key = "ai/image_understanding_execution_allowed";
constexpr auto image_understanding_background_key = "ai/image_understanding_background_enabled";
constexpr auto image_understanding_scope_key = "ai/image_understanding_scan_scope";
constexpr auto image_understanding_minimum_rating_key = "ai/image_understanding_minimum_rating";
constexpr auto image_understanding_auto_keywords_key = "ai/image_understanding_auto_apply_keywords";
constexpr auto raw_denoise_default_amount_key = "ai/raw_denoise_default_amount";

[[nodiscard]] QString normalized_image_understanding_scope(const QString& value) {
    const QString normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("all") || normalized == QStringLiteral("liked")
        || normalized == QStringLiteral("minimum_rating")
        || normalized == QStringLiteral("liked_or_minimum_rating")) {
        return normalized;
    }
    return QStringLiteral("liked");
}

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
    auto_start_enabled_ = settings_->value(QStringLiteral("ai/auto_start_enabled"), false).toBool();
    raw_denoise_execution_allowed_ =
        settings_
            ->value(QString::fromLatin1(raw_denoise_allowed_key), raw_denoise_execution_allowed_)
            .toBool();
    subject_mask_execution_allowed_ =
        settings_
            ->value(QString::fromLatin1(subject_mask_allowed_key), subject_mask_execution_allowed_)
            .toBool();
    image_completion_execution_allowed_ = settings_
                                              ->value(
                                                  QString::fromLatin1(image_completion_allowed_key),
                                                  image_completion_execution_allowed_
                                              )
                                              .toBool();
    const int stored_people_consent_revision =
        settings_->value(QString::fromLatin1(people_analysis_consent_revision_key), 0).toInt();
    const QString stored_people_consent_state =
        settings_
            ->value(
                QString::fromLatin1(people_analysis_consent_state_key),
                people_analysis_consent_state_
            )
            .toString();
    if (stored_people_consent_revision == people_analysis_consent_revision
        && (stored_people_consent_state == QStringLiteral("granted")
            || stored_people_consent_state == QStringLiteral("denied"))) {
        people_analysis_consent_state_ = stored_people_consent_state;
    }
    image_understanding_execution_allowed_ =
        settings_
            ->value(
                QString::fromLatin1(image_understanding_allowed_key),
                image_understanding_execution_allowed_
            )
            .toBool();
    image_understanding_background_enabled_ =
        settings_
            ->value(
                QString::fromLatin1(image_understanding_background_key),
                image_understanding_background_enabled_
            )
            .toBool();
    image_understanding_scan_scope_ = normalized_image_understanding_scope(
        settings_
            ->value(
                QString::fromLatin1(image_understanding_scope_key),
                image_understanding_scan_scope_
            )
            .toString()
    );
    image_understanding_minimum_rating_ = std::clamp(
        settings_
            ->value(
                QString::fromLatin1(image_understanding_minimum_rating_key),
                image_understanding_minimum_rating_
            )
            .toInt(),
        1,
        5
    );
    image_understanding_auto_apply_keywords_ =
        settings_
            ->value(
                QString::fromLatin1(image_understanding_auto_keywords_key),
                image_understanding_auto_apply_keywords_
            )
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

bool AiPreferences::imageCompletionExecutionAllowed() const noexcept {
    return image_completion_execution_allowed_;
}

bool AiPreferences::peopleAnalysisExecutionAllowed() const noexcept {
    return people_analysis_consent_state_ == QStringLiteral("granted");
}

bool AiPreferences::peopleAnalysisConsentDecided() const noexcept {
    return people_analysis_consent_state_ != QStringLiteral("not_asked");
}

QString AiPreferences::peopleAnalysisConsentState() const {
    return people_analysis_consent_state_;
}

bool AiPreferences::imageUnderstandingExecutionAllowed() const noexcept {
    return image_understanding_execution_allowed_;
}

bool AiPreferences::imageUnderstandingBackgroundEnabled() const noexcept {
    return image_understanding_background_enabled_;
}

QString AiPreferences::imageUnderstandingScanScope() const {
    return image_understanding_scan_scope_;
}

int AiPreferences::imageUnderstandingMinimumRating() const noexcept {
    return image_understanding_minimum_rating_;
}

bool AiPreferences::imageUnderstandingAutoApplyKeywords() const noexcept {
    return image_understanding_auto_apply_keywords_;
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

void AiPreferences::setImageCompletionExecutionAllowed(const bool allowed) {
    if (image_completion_execution_allowed_ == allowed) {
        return;
    }
    image_completion_execution_allowed_ = allowed;
    persist(image_completion_allowed_key, allowed);
    emit imageCompletionExecutionAllowedChanged();
}

void AiPreferences::grantPeopleAnalysisConsent() {
    if (people_analysis_consent_state_ == QStringLiteral("granted")) {
        return;
    }
    people_analysis_consent_state_ = QStringLiteral("granted");
    persist(people_analysis_consent_state_key, people_analysis_consent_state_);
    persist(people_analysis_consent_revision_key, people_analysis_consent_revision);
    emit peopleAnalysisConsentChanged();
}

void AiPreferences::denyPeopleAnalysisConsent() {
    if (people_analysis_consent_state_ == QStringLiteral("denied")) {
        return;
    }
    people_analysis_consent_state_ = QStringLiteral("denied");
    persist(people_analysis_consent_state_key, people_analysis_consent_state_);
    persist(people_analysis_consent_revision_key, people_analysis_consent_revision);
    emit peopleAnalysisConsentChanged();
}

void AiPreferences::revokePeopleAnalysisConsent() {
    denyPeopleAnalysisConsent();
}

void AiPreferences::setImageUnderstandingExecutionAllowed(const bool allowed) {
    if (image_understanding_execution_allowed_ == allowed) {
        return;
    }
    image_understanding_execution_allowed_ = allowed;
    persist(image_understanding_allowed_key, allowed);
    emit imageUnderstandingExecutionAllowedChanged();
}

void AiPreferences::setImageUnderstandingBackgroundEnabled(const bool enabled) {
    if (image_understanding_background_enabled_ == enabled) {
        return;
    }
    image_understanding_background_enabled_ = enabled;
    persist(image_understanding_background_key, enabled);
    emit imageUnderstandingBackgroundEnabledChanged();
}

void AiPreferences::setImageUnderstandingScanScope(const QString& scope) {
    const QString normalized = normalized_image_understanding_scope(scope);
    if (image_understanding_scan_scope_ == normalized) {
        return;
    }
    image_understanding_scan_scope_ = normalized;
    persist(image_understanding_scope_key, normalized);
    emit imageUnderstandingScanScopeChanged();
}

void AiPreferences::setImageUnderstandingMinimumRating(const int rating) {
    const int normalized = std::clamp(rating, 1, 5);
    if (image_understanding_minimum_rating_ == normalized) {
        return;
    }
    image_understanding_minimum_rating_ = normalized;
    persist(image_understanding_minimum_rating_key, normalized);
    emit imageUnderstandingMinimumRatingChanged();
}

void AiPreferences::setImageUnderstandingAutoApplyKeywords(const bool enabled) {
    if (image_understanding_auto_apply_keywords_ == enabled) {
        return;
    }
    image_understanding_auto_apply_keywords_ = enabled;
    persist(image_understanding_auto_keywords_key, enabled);
    emit imageUnderstandingAutoApplyKeywordsChanged();
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

void AiPreferences::setAutoStartEnabled(bool enabled) {
    if (auto_start_enabled_ == enabled)
        return;
    auto_start_enabled_ = enabled;
    persist("ai/auto_start_enabled", enabled);
    emit autoStartEnabledChanged();
}
