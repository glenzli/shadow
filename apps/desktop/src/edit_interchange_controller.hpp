#pragma once

#include "backend/edit_types.hpp"
#include "xmp_develop_import.hpp"

#include <QObject>
#include <QUrl>
#include <QVariantList>

class EditController;
class DesktopBackend;

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
    Q_PROPERTY(bool recipeReady READ recipeReady NOTIFY recipePreviewChanged)
    Q_PROPERTY(bool recipeCanApply READ recipeCanApply NOTIFY recipePreviewChanged)
    Q_PROPERTY(QString recipeSourceName READ recipeSourceName NOTIFY recipePreviewChanged)
    Q_PROPERTY(QString recipeLabel READ recipeLabel NOTIFY recipePreviewChanged)
    Q_PROPERTY(int recipeGradeNodeCount READ recipeGradeNodeCount NOTIFY recipePreviewChanged)
    Q_PROPERTY(int recipePortableMaskCount READ recipePortableMaskCount NOTIFY recipePreviewChanged)
    Q_PROPERTY(QVariantList recipeWarnings READ recipeWarnings NOTIFY recipePreviewChanged)
    Q_PROPERTY(QString recipeErrorText READ recipeErrorText NOTIFY recipePreviewChanged)
    Q_PROPERTY(
        QString recipeApplyErrorText READ recipeApplyErrorText NOTIFY recipeApplyErrorChanged
    )
    Q_PROPERTY(
        QString recipeExportErrorText READ recipeExportErrorText NOTIFY recipeExportErrorChanged
    )

  public:
    explicit EditInterchangeController(
        DesktopBackend& backend,
        EditController& editor,
        QObject* parent = nullptr
    );

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] bool canApply() const noexcept;
    [[nodiscard]] QString sourceName() const;
    [[nodiscard]] QString processVersion() const;
    [[nodiscard]] QVariantList mappedAdjustments() const;
    [[nodiscard]] QVariantList ignoredFields() const;
    [[nodiscard]] QVariantList invalidFields() const;
    [[nodiscard]] QString errorText() const;
    [[nodiscard]] QString applyErrorText() const;
    [[nodiscard]] bool recipeReady() const noexcept;
    [[nodiscard]] bool recipeCanApply() const noexcept;
    [[nodiscard]] QString recipeSourceName() const;
    [[nodiscard]] QString recipeLabel() const;
    [[nodiscard]] int recipeGradeNodeCount() const noexcept;
    [[nodiscard]] int recipePortableMaskCount() const noexcept;
    [[nodiscard]] QVariantList recipeWarnings() const;
    [[nodiscard]] QString recipeErrorText() const;
    [[nodiscard]] QString recipeApplyErrorText() const;
    [[nodiscard]] QString recipeExportErrorText() const;

    Q_INVOKABLE void clear();
    Q_INVOKABLE void previewXmp(const QUrl& file_url);
    Q_INVOKABLE bool applyXmp();
    Q_INVOKABLE void clearShadowRecipe();
    Q_INVOKABLE void previewShadowRecipe(const QUrl& file_url);
    Q_INVOKABLE bool applyShadowRecipe();
    Q_INVOKABLE bool exportShadowRecipe(const QUrl& file_url, const QString& label);

  signals:
    void previewChanged();
    void applyErrorChanged();
    void xmpApplied();
    void recipePreviewChanged();
    void recipeApplyErrorChanged();
    void recipeExportErrorChanged();
    void shadowRecipeApplied();
    void shadowRecipeExported(QString path);

  private:
    [[nodiscard]] static QString targetName(XmpDevelopTarget target);
    [[nodiscard]] static QString targetValue(const XmpDevelopAdjustment& adjustment);
    [[nodiscard]] static QString ignoredReason(XmpDevelopIgnoredReason reason);
    void setApplyError(QString error);
    void setRecipeApplyError(QString error);
    void setRecipeExportError(QString error);

    DesktopBackend& backend_;
    EditController& editor_;
    QString source_name_;
    XmpDevelopImport preview_;
    QString preview_error_;
    QString apply_error_;
    bool ready_ = false;
    QString recipe_source_name_;
    BackendShadowRecipeImportPreview recipe_preview_;
    QString recipe_preview_error_;
    QString recipe_apply_error_;
    QString recipe_export_error_;
    QString recipe_target_photo_id_;
    QString recipe_target_base_commit_id_;
    bool recipe_ready_ = false;
};
