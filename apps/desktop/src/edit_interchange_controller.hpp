#pragma once

#include "xmp_develop_import.hpp"

#include <QObject>
#include <QUrl>
#include <QVariantList>

class EditController;

/// Owns format-boundary preview and application. External adjustment formats
/// never become a second edit authority: accepted values are materialized into
/// one ordinary photo-local Grade Node.
class EditInterchangeController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool ready READ ready NOTIFY previewChanged)
    Q_PROPERTY(bool canApply READ canApply NOTIFY previewChanged)
    Q_PROPERTY(QString sourceName READ sourceName NOTIFY previewChanged)
    Q_PROPERTY(QString processVersion READ processVersion NOTIFY previewChanged)
    Q_PROPERTY(QVariantList mappedAdjustments READ mappedAdjustments NOTIFY previewChanged)
    Q_PROPERTY(QVariantList ignoredFields READ ignoredFields NOTIFY previewChanged)
    Q_PROPERTY(QVariantList invalidFields READ invalidFields NOTIFY previewChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY previewChanged)
    Q_PROPERTY(QString applyErrorText READ applyErrorText NOTIFY applyErrorChanged)

  public:
    explicit EditInterchangeController(EditController& editor, QObject* parent = nullptr);

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] bool canApply() const noexcept;
    [[nodiscard]] QString sourceName() const;
    [[nodiscard]] QString processVersion() const;
    [[nodiscard]] QVariantList mappedAdjustments() const;
    [[nodiscard]] QVariantList ignoredFields() const;
    [[nodiscard]] QVariantList invalidFields() const;
    [[nodiscard]] QString errorText() const;
    [[nodiscard]] QString applyErrorText() const;

    Q_INVOKABLE void clear();
    Q_INVOKABLE void previewXmp(const QUrl& file_url);
    Q_INVOKABLE bool applyXmp();

  signals:
    void previewChanged();
    void applyErrorChanged();
    void xmpApplied();

  private:
    [[nodiscard]] static QString targetName(XmpDevelopTarget target);
    [[nodiscard]] static QString targetValue(const XmpDevelopAdjustment& adjustment);
    [[nodiscard]] static QString ignoredReason(XmpDevelopIgnoredReason reason);
    void setApplyError(QString error);

    EditController& editor_;
    QString source_name_;
    XmpDevelopImport preview_;
    QString preview_error_;
    QString apply_error_;
    bool ready_ = false;
};
