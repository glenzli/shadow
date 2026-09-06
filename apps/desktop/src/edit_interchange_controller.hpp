#pragma once

#include "backend/edit_types.hpp"
#include "xmp_develop_import.hpp"

#include <QFutureWatcher>
#include <QHash>
#include <QObject>
#include <QUrl>
#include <QVariantList>

#include <atomic>
#include <cstdint>
#include <mutex>

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
    Q_PROPERTY(QStringList compatibilityWarnings READ compatibilityWarnings NOTIFY previewChanged)
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
    Q_PROPERTY(int recipeSemanticItemCount READ recipeSemanticItemCount NOTIFY recipePreviewChanged)
    Q_PROPERTY(
        int recipeSemanticCompletedCount READ recipeSemanticCompletedCount NOTIFY
            recipePreviewChanged
    )
    Q_PROPERTY(
        int recipeSemanticFailedCount READ recipeSemanticFailedCount NOTIFY recipePreviewChanged
    )
    Q_PROPERTY(
        QVariantList recipeSemanticItems READ recipeSemanticItems NOTIFY recipePreviewChanged
    )
    Q_PROPERTY(
        bool recipeAdaptationRunning READ recipeAdaptationRunning NOTIFY recipePreviewChanged
    )
    Q_PROPERTY(
        bool recipeAdaptationPartial READ recipeAdaptationPartial NOTIFY recipePreviewChanged
    )
    Q_PROPERTY(bool recipeCanAdapt READ recipeCanAdapt NOTIFY recipePreviewChanged)
    Q_PROPERTY(bool recipeCanRetryFailed READ recipeCanRetryFailed NOTIFY recipePreviewChanged)
    Q_PROPERTY(
        bool recipeCanApplyAvailableNodes READ recipeCanApplyAvailableNodes NOTIFY
            recipePreviewChanged
    )
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
    ~EditInterchangeController() override;

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] bool canApply() const noexcept;
    [[nodiscard]] QString sourceName() const;
    [[nodiscard]] QString processVersion() const;
    [[nodiscard]] QStringList compatibilityWarnings() const;
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
    [[nodiscard]] int recipeSemanticItemCount() const noexcept;
    [[nodiscard]] int recipeSemanticCompletedCount() const noexcept;
    [[nodiscard]] int recipeSemanticFailedCount() const noexcept;
    [[nodiscard]] QVariantList recipeSemanticItems() const;
    [[nodiscard]] bool recipeAdaptationRunning() const noexcept;
    [[nodiscard]] bool recipeAdaptationPartial() const noexcept;
    [[nodiscard]] bool recipeCanAdapt() const noexcept;
    [[nodiscard]] bool recipeCanRetryFailed() const noexcept;
    [[nodiscard]] bool recipeCanApplyAvailableNodes() const noexcept;
    [[nodiscard]] QString recipeApplyErrorText() const;
    [[nodiscard]] QString recipeExportErrorText() const;

    Q_INVOKABLE void clear();
    Q_INVOKABLE void previewXmp(const QUrl& file_url);
    Q_INVOKABLE bool applyXmp();
    Q_INVOKABLE void clearShadowRecipe();
    Q_INVOKABLE void previewShadowRecipe(const QUrl& file_url);
    Q_INVOKABLE bool applyShadowRecipe();
    Q_INVOKABLE bool startShadowRecipeAdaptation();
    Q_INVOKABLE void cancelShadowRecipeAdaptation();
    Q_INVOKABLE bool retryFailedShadowRecipeItems();
    Q_INVOKABLE bool applyAvailableShadowRecipeNodes();
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
    void finishRecipeAdaptation();
    void closeRecipePlan() noexcept;
    [[nodiscard]] bool recipeTargetIsCurrent() const noexcept;

    enum class RecipeAdaptationMode : std::uint8_t { All, RetryFailed, ApplyAvailable };
    struct RecipeAdaptationResult final {
        BackendRecipeImportPlan plan;
        BackendGradeStack grade_stack;
        QString error;
        std::uint64_t controller_generation = 0;
        bool finalized = false;
    };
    [[nodiscard]] bool startRecipeAdaptation(RecipeAdaptationMode mode);

    DesktopBackend& backend_;
    EditController& editor_;
    QString source_name_;
    QByteArray xmp_digest_;
    QHash<QString, QString> applied_xmp_nodes_;
    XmpDevelopImport preview_;
    QString preview_error_;
    QString apply_error_;
    bool ready_ = false;
    QString recipe_source_name_;
    BackendShadowRecipeImportPreview recipe_preview_;
    BackendRecipeImportPlan recipe_import_plan_;
    QString recipe_preview_error_;
    QString recipe_apply_error_;
    QString recipe_export_error_;
    QString recipe_target_photo_id_;
    QString recipe_target_source_path_;
    QString recipe_target_base_commit_id_;
    QString recipe_target_working_commit_id_;
    BackendGradeStack recipe_target_grade_stack_;
    QFutureWatcher<RecipeAdaptationResult> recipe_adaptation_watcher_;
    std::atomic<std::uint64_t> recipe_controller_generation_{0};
    mutable std::mutex recipe_job_mutex_;
    QString recipe_active_item_id_;
    std::uint64_t recipe_active_job_token_ = 0;
    bool recipe_ready_ = false;
};
