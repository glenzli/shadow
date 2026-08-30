#pragma once

#include <QObject>
#include <QString>
#include <QUrl>

#include <memory>

class QSettings;
class QVariant;

/// Persistent user policy for optional local AI execution.
///
/// Model discovery and verification remain owned by the Rust runtimes. This
/// object controls whether a new model job may be admitted and the authoring
/// default used when a new AI RAW Denoise node is created.
class AiPreferences final : public QObject {
    Q_OBJECT
    Q_PROPERTY(
        bool rawDenoiseExecutionAllowed READ rawDenoiseExecutionAllowed WRITE
            setRawDenoiseExecutionAllowed NOTIFY rawDenoiseExecutionAllowedChanged
    )
    Q_PROPERTY(
        bool subjectMaskExecutionAllowed READ subjectMaskExecutionAllowed WRITE
            setSubjectMaskExecutionAllowed NOTIFY subjectMaskExecutionAllowedChanged
    )
    Q_PROPERTY(
        bool imageCompletionExecutionAllowed READ imageCompletionExecutionAllowed WRITE
            setImageCompletionExecutionAllowed NOTIFY imageCompletionExecutionAllowedChanged
    )
    Q_PROPERTY(
        bool peopleAnalysisExecutionAllowed READ peopleAnalysisExecutionAllowed NOTIFY
            peopleAnalysisConsentChanged
    )
    Q_PROPERTY(
        bool peopleAnalysisConsentDecided READ peopleAnalysisConsentDecided NOTIFY
            peopleAnalysisConsentChanged
    )
    Q_PROPERTY(
        QString peopleAnalysisConsentState READ peopleAnalysisConsentState NOTIFY
            peopleAnalysisConsentChanged
    )
    Q_PROPERTY(
        bool imageUnderstandingExecutionAllowed READ imageUnderstandingExecutionAllowed WRITE
            setImageUnderstandingExecutionAllowed NOTIFY imageUnderstandingExecutionAllowedChanged
    )
    Q_PROPERTY(
        bool imageUnderstandingBackgroundEnabled READ imageUnderstandingBackgroundEnabled WRITE
            setImageUnderstandingBackgroundEnabled NOTIFY imageUnderstandingBackgroundEnabledChanged
    )
    Q_PROPERTY(
        QString imageUnderstandingScanScope READ imageUnderstandingScanScope WRITE
            setImageUnderstandingScanScope NOTIFY imageUnderstandingScanScopeChanged
    )
    Q_PROPERTY(
        int imageUnderstandingMinimumRating READ imageUnderstandingMinimumRating WRITE
            setImageUnderstandingMinimumRating NOTIFY imageUnderstandingMinimumRatingChanged
    )
    Q_PROPERTY(
        bool imageUnderstandingAutoApplyKeywords READ imageUnderstandingAutoApplyKeywords WRITE
            setImageUnderstandingAutoApplyKeywords NOTIFY imageUnderstandingAutoApplyKeywordsChanged
    )
    Q_PROPERTY(
        int rawDenoiseDefaultAmount READ rawDenoiseDefaultAmount WRITE setRawDenoiseDefaultAmount
            NOTIFY rawDenoiseDefaultAmountChanged
    )
    Q_PROPERTY(QString modelStoragePath READ modelStoragePath CONSTANT)
    Q_PROPERTY(QUrl modelStorageUrl READ modelStorageUrl CONSTANT)

  public:
    explicit AiPreferences(
        const QString& application_data_root,
        const QString& isolated_settings_file = {},
        QObject* parent = nullptr
    );
    ~AiPreferences() override;

    AiPreferences(const AiPreferences&) = delete;
    AiPreferences& operator=(const AiPreferences&) = delete;

    [[nodiscard]] bool rawDenoiseExecutionAllowed() const noexcept;
    [[nodiscard]] bool subjectMaskExecutionAllowed() const noexcept;
    [[nodiscard]] bool imageCompletionExecutionAllowed() const noexcept;
    [[nodiscard]] bool peopleAnalysisExecutionAllowed() const noexcept;
    [[nodiscard]] bool peopleAnalysisConsentDecided() const noexcept;
    [[nodiscard]] QString peopleAnalysisConsentState() const;
    [[nodiscard]] bool imageUnderstandingExecutionAllowed() const noexcept;
    [[nodiscard]] bool imageUnderstandingBackgroundEnabled() const noexcept;
    [[nodiscard]] QString imageUnderstandingScanScope() const;
    [[nodiscard]] int imageUnderstandingMinimumRating() const noexcept;
    [[nodiscard]] bool imageUnderstandingAutoApplyKeywords() const noexcept;
    [[nodiscard]] int rawDenoiseDefaultAmount() const noexcept;
    [[nodiscard]] QString modelStoragePath() const;
    [[nodiscard]] QUrl modelStorageUrl() const;

    void setRawDenoiseExecutionAllowed(bool allowed);
    void setSubjectMaskExecutionAllowed(bool allowed);
    void setImageCompletionExecutionAllowed(bool allowed);
    Q_INVOKABLE void grantPeopleAnalysisConsent();
    Q_INVOKABLE void denyPeopleAnalysisConsent();
    Q_INVOKABLE void revokePeopleAnalysisConsent();
    void setImageUnderstandingExecutionAllowed(bool allowed);
    void setImageUnderstandingBackgroundEnabled(bool enabled);
    void setImageUnderstandingScanScope(const QString& scope);
    void setImageUnderstandingMinimumRating(int rating);
    void setImageUnderstandingAutoApplyKeywords(bool enabled);
    void setRawDenoiseDefaultAmount(int amount_percent);

  signals:
    void rawDenoiseExecutionAllowedChanged();
    void subjectMaskExecutionAllowedChanged();
    void imageCompletionExecutionAllowedChanged();
    void peopleAnalysisConsentChanged();
    void imageUnderstandingExecutionAllowedChanged();
    void imageUnderstandingBackgroundEnabledChanged();
    void imageUnderstandingScanScopeChanged();
    void imageUnderstandingMinimumRatingChanged();
    void imageUnderstandingAutoApplyKeywordsChanged();
    void rawDenoiseDefaultAmountChanged();

  private:
    void persist(const char* key, const QVariant& value);

    std::unique_ptr<QSettings> settings_;
    QString model_storage_path_;
    bool raw_denoise_execution_allowed_ = true;
    bool subject_mask_execution_allowed_ = true;
    bool image_completion_execution_allowed_ = false;
    QString people_analysis_consent_state_ = QStringLiteral("not_asked");
    bool image_understanding_execution_allowed_ = true;
    bool image_understanding_background_enabled_ = false;
    QString image_understanding_scan_scope_ = QStringLiteral("liked");
    int image_understanding_minimum_rating_ = 5;
    bool image_understanding_auto_apply_keywords_ = false;
    int raw_denoise_default_amount_ = 100;
};
