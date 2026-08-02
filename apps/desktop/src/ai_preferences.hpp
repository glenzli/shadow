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
    [[nodiscard]] int rawDenoiseDefaultAmount() const noexcept;
    [[nodiscard]] QString modelStoragePath() const;
    [[nodiscard]] QUrl modelStorageUrl() const;

    void setRawDenoiseExecutionAllowed(bool allowed);
    void setSubjectMaskExecutionAllowed(bool allowed);
    void setRawDenoiseDefaultAmount(int amount_percent);

  signals:
    void rawDenoiseExecutionAllowedChanged();
    void subjectMaskExecutionAllowedChanged();
    void rawDenoiseDefaultAmountChanged();

  private:
    void persist(const char* key, const QVariant& value);

    std::unique_ptr<QSettings> settings_;
    QString model_storage_path_;
    bool raw_denoise_execution_allowed_ = true;
    bool subject_mask_execution_allowed_ = true;
    int raw_denoise_default_amount_ = 100;
};
