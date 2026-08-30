#include "edit_interchange_controller.hpp"

#include "desktop_backend.hpp"
#include "edit_controller.hpp"

#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QSaveFile>
#include <QVariantMap>

#include <exception>
#include <utility>

namespace {

constexpr qint64 MAXIMUM_XMP_BYTES = 4 * 1024 * 1024;
constexpr qint64 MAXIMUM_SHADOW_RECIPE_BYTES = 16 * 1024 * 1024;

[[nodiscard]] QString number(const double value) {
    return QLocale().toString(value, 'f', 2);
}

} // namespace

EditInterchangeController::EditInterchangeController(
    DesktopBackend& backend,
    EditController& editor,
    QObject* const parent
) : QObject(parent), backend_(backend), editor_(editor) {
    connect(
        &editor_,
        &EditController::activeChanged,
        this,
        &EditInterchangeController::previewChanged
    );
    connect(
        &editor_,
        &EditController::stateBusyChanged,
        this,
        &EditInterchangeController::previewChanged
    );
    connect(
        &editor_,
        &EditController::gradeNodeActionsChanged,
        this,
        &EditInterchangeController::previewChanged
    );
    connect(
        &editor_,
        &EditController::activeChanged,
        this,
        &EditInterchangeController::recipePreviewChanged
    );
    connect(
        &editor_,
        &EditController::stateBusyChanged,
        this,
        &EditInterchangeController::recipePreviewChanged
    );
    connect(
        &editor_,
        &EditController::sourceIdentityChanged,
        this,
        &EditInterchangeController::recipePreviewChanged
    );
    connect(
        &editor_,
        &EditController::editBaseCommitIdChanged,
        this,
        &EditInterchangeController::recipePreviewChanged
    );
}

bool EditInterchangeController::ready() const noexcept {
    return ready_;
}

bool EditInterchangeController::canApply() const noexcept {
    return ready_ && preview_.canApply() && editor_.active() && editor_.canAddGradeNode()
           && !editor_.stateBusy();
}

QString EditInterchangeController::sourceName() const {
    return source_name_;
}

QString EditInterchangeController::processVersion() const {
    return preview_.process_version;
}

QVariantList EditInterchangeController::mappedAdjustments() const {
    QVariantList values;
    values.reserve(preview_.adjustments.size());
    for (const auto& adjustment : preview_.adjustments) {
        values.push_back(
            QVariantMap{
                {QStringLiteral("sourceName"), adjustment.source_name},
                {QStringLiteral("sourceValue"), number(adjustment.source_value)},
                {QStringLiteral("targetName"), targetName(adjustment.target)},
                {QStringLiteral("targetValue"), targetValue(adjustment)},
            }
        );
    }
    return values;
}

QVariantList EditInterchangeController::ignoredFields() const {
    QVariantList values;
    values.reserve(preview_.ignored_fields.size());
    for (const auto& field : preview_.ignored_fields) {
        values.push_back(
            QVariantMap{
                {QStringLiteral("sourceName"), field.source_name},
                {QStringLiteral("reason"), ignoredReason(field.reason)},
            }
        );
    }
    return values;
}

QVariantList EditInterchangeController::invalidFields() const {
    QVariantList values;
    values.reserve(preview_.invalid_fields.size());
    for (const auto& field : preview_.invalid_fields) {
        QString problem;
        if (field.problem == QStringLiteral("conflicting values")) {
            problem = tr("The file contains different values for the same field.");
        } else if (field.problem == QStringLiteral("not a finite number")) {
            problem = tr("The value is not a valid finite number.");
        } else {
            problem = tr("The value is outside the supported range.");
        }
        values.push_back(
            QVariantMap{
                {QStringLiteral("sourceName"), field.source_name},
                {QStringLiteral("sourceValue"), field.source_value},
                {QStringLiteral("problem"), problem},
            }
        );
    }
    return values;
}

QString EditInterchangeController::errorText() const {
    if (!preview_error_.isEmpty()) {
        return preview_error_;
    }
    if (!preview_.document_error.isEmpty()) {
        return tr("This is not a readable XMP document: %1").arg(preview_.document_error);
    }
    if (ready_ && preview_.adjustments.isEmpty() && preview_.invalid_fields.isEmpty()) {
        return tr("No supported Camera Raw develop adjustments were found.");
    }
    return {};
}

QString EditInterchangeController::applyErrorText() const {
    return apply_error_;
}

bool EditInterchangeController::recipeReady() const noexcept {
    return recipe_ready_;
}

bool EditInterchangeController::recipeCanApply() const noexcept {
    return recipe_ready_ && editor_.active() && !editor_.stateBusy()
           && editor_.photoId() == recipe_target_photo_id_
           && editor_.editBaseCommitId() == recipe_target_base_commit_id_;
}

QString EditInterchangeController::recipeSourceName() const {
    return recipe_source_name_;
}

QString EditInterchangeController::recipeLabel() const {
    return recipe_preview_.label;
}

int EditInterchangeController::recipeGradeNodeCount() const noexcept {
    return static_cast<int>(recipe_preview_.grade_node_count);
}

int EditInterchangeController::recipePortableMaskCount() const noexcept {
    return static_cast<int>(recipe_preview_.portable_mask_count);
}

QVariantList EditInterchangeController::recipeWarnings() const {
    QVariantList warnings;
    if (recipe_preview_.managed_mask_node_count > 0) {
        warnings.push_back(
            tr("%1 managed-mask Grade Nodes will be imported disabled. Their source-photo pixels are never reused.")
                .arg(recipe_preview_.managed_mask_node_count)
        );
    }
    if (recipe_preview_.semantic_mask_intent_count > 0) {
        warnings.push_back(
            tr("%1 semantic managed masks will be omitted; their Grade Nodes remain disabled.")
                .arg(recipe_preview_.semantic_mask_intent_count)
        );
    }
    if (recipe_preview_.removed_lut_count > 0) {
        warnings.push_back(
            tr("%1 path-bound LUT references will be omitted because this file does not contain LUT resources.")
                .arg(recipe_preview_.removed_lut_count)
        );
    }
    if (recipe_preview_.detached_shared_node_count > 0) {
        warnings.push_back(
            tr("%1 shared Grade Node links will become independent local nodes.")
                .arg(recipe_preview_.detached_shared_node_count)
        );
    }
    if (recipe_preview_.foundation_omitted || recipe_preview_.raw_denoise_omitted) {
        warnings.push_back(tr("Source development and AI RAW Denoise remain unchanged on this photo."));
    }
    if (recipe_preview_.excluded_retouch_region_count > 0) {
        warnings.push_back(
            tr("%1 source-photo repair regions will not be imported.")
                .arg(recipe_preview_.excluded_retouch_region_count)
        );
    }
    if (recipe_preview_.excluded_completion_region_count > 0) {
        warnings.push_back(
            tr("%1 source-photo AI completion regions will not be imported.")
                .arg(recipe_preview_.excluded_completion_region_count)
        );
    }
    if (recipe_preview_.excluded_liquify_stroke_count > 0) {
        warnings.push_back(
            tr("%1 source-photo Liquify strokes will not be imported.")
                .arg(recipe_preview_.excluded_liquify_stroke_count)
        );
    }
    if (recipe_preview_.canvas_omitted) {
        warnings.push_back(tr("Crop, orientation, and perspective remain unchanged on this photo."));
    }
    return warnings;
}

QString EditInterchangeController::recipeErrorText() const {
    return recipe_preview_error_;
}

QString EditInterchangeController::recipeApplyErrorText() const {
    return recipe_apply_error_;
}

QString EditInterchangeController::recipeExportErrorText() const {
    return recipe_export_error_;
}

void EditInterchangeController::clear() {
    source_name_.clear();
    preview_ = {};
    preview_error_.clear();
    ready_ = false;
    setApplyError({});
    emit previewChanged();
}

void EditInterchangeController::previewXmp(const QUrl& file_url) {
    source_name_.clear();
    preview_ = {};
    preview_error_.clear();
    ready_ = true;
    setApplyError({});

    const QString path = file_url.toLocalFile();
    const QFileInfo info(path);
    source_name_ = info.fileName();
    if (path.isEmpty() || !info.isFile()) {
        preview_error_ = tr("The selected file is unavailable.");
        emit previewChanged();
        return;
    }
    if (info.size() > MAXIMUM_XMP_BYTES) {
        preview_error_ = tr("The XMP file is larger than 4 MB.");
        emit previewChanged();
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        preview_error_ = tr("The selected file could not be opened.");
        emit previewChanged();
        return;
    }
    preview_ = parseXmpDevelopImport(file.readAll());
    emit previewChanged();
}

bool EditInterchangeController::applyXmp() {
    setApplyError({});
    if (!preview_.canApply()) {
        setApplyError(tr("Resolve the XMP preview before importing."));
        return false;
    }
    if (!editor_.active()) {
        setApplyError(tr("Open a photo before importing XMP adjustments."));
        return false;
    }
    if (editor_.stateBusy()) {
        setApplyError(tr("Wait for the current edit operation to finish."));
        return false;
    }
    if (!editor_.canAddGradeNode()) {
        setApplyError(tr("This photo already contains the maximum of 16 Grade Nodes."));
        return false;
    }

    const QString previous_node_id = editor_.selectedGradeNodeId();
    editor_.addGradeNode();
    if (!editor_.hasSelectedGradeNode() || editor_.selectedGradeNodeId().isEmpty()
        || editor_.selectedGradeNodeId() == previous_node_id) {
        setApplyError(tr("Shadow could not create the destination Grade Node."));
        return false;
    }

    for (const auto& adjustment : preview_.adjustments) {
        switch (adjustment.target) {
        case XmpDevelopTarget::ExposureStops:
            editor_.setExposureStops(adjustment.target_value);
            break;
        case XmpDevelopTarget::ContrastFactor:
            editor_.setContrastFactor(adjustment.target_value);
            break;
        case XmpDevelopTarget::SaturationFactor:
            editor_.setSaturationFactor(adjustment.target_value);
            break;
        case XmpDevelopTarget::Highlights:
            editor_.setParameterValue(QStringLiteral("highlights"), adjustment.target_value);
            break;
        case XmpDevelopTarget::Shadows:
            editor_.setParameterValue(QStringLiteral("shadows"), adjustment.target_value);
            break;
        case XmpDevelopTarget::Whites:
            editor_.setParameterValue(QStringLiteral("whites"), adjustment.target_value);
            break;
        case XmpDevelopTarget::Blacks:
            editor_.setParameterValue(QStringLiteral("blacks"), adjustment.target_value);
            break;
        case XmpDevelopTarget::Texture:
            editor_.setParameterValue(QStringLiteral("texture"), adjustment.target_value);
            break;
        case XmpDevelopTarget::Clarity:
            editor_.setParameterValue(QStringLiteral("clarity"), adjustment.target_value);
            break;
        case XmpDevelopTarget::Dehaze:
            editor_.setParameterValue(QStringLiteral("dehaze"), adjustment.target_value);
            break;
        case XmpDevelopTarget::Vibrance:
            editor_.setParameterValue(QStringLiteral("vibrance"), adjustment.target_value);
            break;
        }
    }
    emit xmpApplied();
    return true;
}

void EditInterchangeController::clearShadowRecipe() {
    recipe_source_name_.clear();
    recipe_preview_ = {};
    recipe_preview_error_.clear();
    recipe_target_photo_id_.clear();
    recipe_target_base_commit_id_.clear();
    recipe_ready_ = false;
    setRecipeApplyError({});
    setRecipeExportError({});
    emit recipePreviewChanged();
}

void EditInterchangeController::previewShadowRecipe(const QUrl& file_url) {
    clearShadowRecipe();
    const QString path = file_url.toLocalFile();
    const QFileInfo info(path);
    recipe_source_name_ = info.fileName();
    if (path.isEmpty() || !info.isFile()) {
        recipe_preview_error_ = tr("The selected Shadow Recipe file is unavailable.");
        emit recipePreviewChanged();
        return;
    }
    if (info.size() > MAXIMUM_SHADOW_RECIPE_BYTES) {
        recipe_preview_error_ = tr("The Shadow Recipe file is larger than 16 MB.");
        emit recipePreviewChanged();
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        recipe_preview_error_ = tr("The selected Shadow Recipe file could not be opened.");
        emit recipePreviewChanged();
        return;
    }
    try {
        recipe_preview_ = backend_.previewShadowRecipe(file.readAll());
        recipe_target_photo_id_ = editor_.photoId();
        recipe_target_base_commit_id_ = editor_.editBaseCommitId();
        recipe_ready_ = true;
    } catch (const std::exception& error) {
        recipe_preview_error_ =
            tr("Shadow could not read this Recipe: %1").arg(QString::fromUtf8(error.what()));
    }
    emit recipePreviewChanged();
}

bool EditInterchangeController::applyShadowRecipe() {
    setRecipeApplyError({});
    if (!recipe_ready_) {
        setRecipeApplyError(tr("Choose a valid Shadow Recipe before importing."));
        return false;
    }
    if (editor_.photoId() != recipe_target_photo_id_
        || editor_.editBaseCommitId() != recipe_target_base_commit_id_) {
        setRecipeApplyError(tr("The open photo changed after this Recipe was previewed. Preview it again."));
        return false;
    }
    QString error;
    if (!editor_.applyShadowRecipeGradeNodes(recipe_preview_.portable_grade_stack, &error)) {
        setRecipeApplyError(std::move(error));
        return false;
    }
    emit shadowRecipeApplied();
    return true;
}

bool EditInterchangeController::exportShadowRecipe(
    const QUrl& file_url,
    const QString& label
) {
    setRecipeExportError({});
    if (!editor_.active()) {
        setRecipeExportError(tr("Open a photo before exporting a Shadow Recipe."));
        return false;
    }
    if (editor_.stateBusy()) {
        setRecipeExportError(tr("Wait for the current edit operation to finish."));
        return false;
    }
    QString path = file_url.toLocalFile();
    if (path.isEmpty()) {
        setRecipeExportError(tr("Choose a local destination for the Shadow Recipe."));
        return false;
    }
    if (QFileInfo(path).suffix().isEmpty()) {
        path += QStringLiteral(".shadowrecipe");
    }

    QByteArray document;
    try {
        document = backend_.exportShadowRecipe(
            editor_.photoId(),
            editor_.sourcePath(),
            editor_.editBaseCommitId(),
            editor_.gradeStackForInterchange(),
            label.trimmed()
        );
    } catch (const std::exception& error) {
        setRecipeExportError(
            tr("Shadow could not create this Recipe: %1").arg(QString::fromUtf8(error.what()))
        );
        return false;
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        setRecipeExportError(tr("The Shadow Recipe destination could not be opened."));
        return false;
    }
    if (file.write(document) != document.size()) {
        file.cancelWriting();
        setRecipeExportError(tr("The complete Shadow Recipe could not be written."));
        return false;
    }
    if (!file.commit()) {
        setRecipeExportError(tr("The Shadow Recipe could not be published atomically."));
        return false;
    }
    editor_.reportShadowRecipeExported(QFileInfo(path).fileName());
    emit shadowRecipeExported(path);
    return true;
}

QString EditInterchangeController::targetName(const XmpDevelopTarget target) {
    switch (target) {
    case XmpDevelopTarget::ExposureStops:
        return tr("Exposure");
    case XmpDevelopTarget::ContrastFactor:
        return tr("Contrast");
    case XmpDevelopTarget::Highlights:
        return tr("Highlights");
    case XmpDevelopTarget::Shadows:
        return tr("Shadows");
    case XmpDevelopTarget::Whites:
        return tr("Whites");
    case XmpDevelopTarget::Blacks:
        return tr("Blacks");
    case XmpDevelopTarget::Texture:
        return tr("Texture");
    case XmpDevelopTarget::Clarity:
        return tr("Clarity");
    case XmpDevelopTarget::Dehaze:
        return tr("Dehaze");
    case XmpDevelopTarget::Vibrance:
        return tr("Vibrance");
    case XmpDevelopTarget::SaturationFactor:
        return tr("Chroma");
    }
    return {};
}

QString EditInterchangeController::targetValue(const XmpDevelopAdjustment& adjustment) {
    if (adjustment.target == XmpDevelopTarget::ExposureStops) {
        return tr("%1 EV").arg(number(adjustment.target_value));
    }
    if (adjustment.target == XmpDevelopTarget::ContrastFactor
        || adjustment.target == XmpDevelopTarget::SaturationFactor) {
        return tr("%1×").arg(number(adjustment.target_value));
    }
    return tr("%1%").arg(number(adjustment.target_value * 100.0));
}

QString EditInterchangeController::ignoredReason(const XmpDevelopIgnoredReason reason) {
    switch (reason) {
    case XmpDevelopIgnoredReason::AbsoluteWhiteBalance:
        return tr("Absolute source white balance has no reliable Grade Node equivalent.");
    case XmpDevelopIgnoredReason::StructuredAdjustment:
        return tr("This structured adjustment is outside the controlled mapping.");
    case XmpDevelopIgnoredReason::OutsideControlledMapping:
        return tr("This field is outside the controlled mapping.");
    }
    return {};
}

void EditInterchangeController::setApplyError(QString error) {
    if (apply_error_ == error) {
        return;
    }
    apply_error_ = std::move(error);
    emit applyErrorChanged();
}

void EditInterchangeController::setRecipeApplyError(QString error) {
    if (recipe_apply_error_ == error) {
        return;
    }
    recipe_apply_error_ = std::move(error);
    emit recipeApplyErrorChanged();
}

void EditInterchangeController::setRecipeExportError(QString error) {
    if (recipe_export_error_ == error) {
        return;
    }
    recipe_export_error_ = std::move(error);
    emit recipeExportErrorChanged();
}
