#include "edit_interchange_controller.hpp"

#include "desktop_backend.hpp"
#include "edit_controller.hpp"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QSaveFile>
#include <QSet>
#include <QVariantMap>
#include <QtConcurrent>

#include <algorithm>
#include <exception>
#include <utility>

namespace {

constexpr qint64 MAXIMUM_XMP_BYTES = 4 * 1024 * 1024;
constexpr qint64 MAXIMUM_SHADOW_RECIPE_BYTES = 16 * 1024 * 1024;

[[nodiscard]] QString number(const double value) {
    return QLocale().toString(value, 'f', 2);
}

[[nodiscard]] QString recipe_item_state(const BackendRecipeImportItemTerminal terminal) {
    switch (terminal) {
    case BackendRecipeImportItemTerminal::Pending:
        return QStringLiteral("waiting");
    case BackendRecipeImportItemTerminal::Running:
        return QStringLiteral("preparing");
    case BackendRecipeImportItemTerminal::Staged:
        return QStringLiteral("segmenting");
    case BackendRecipeImportItemTerminal::Completed:
        return QStringLiteral("completed");
    case BackendRecipeImportItemTerminal::NotFound:
        return QStringLiteral("notFound");
    case BackendRecipeImportItemTerminal::Unavailable:
        return QStringLiteral("unavailable");
    case BackendRecipeImportItemTerminal::Cancelled:
        return QStringLiteral("cancelled");
    case BackendRecipeImportItemTerminal::Failed:
        return QStringLiteral("failed");
    }
    return QStringLiteral("failed");
}

[[nodiscard]] bool recipe_item_failed(const BackendRecipeImportItemTerminal terminal) {
    return terminal == BackendRecipeImportItemTerminal::NotFound
           || terminal == BackendRecipeImportItemTerminal::Unavailable
           || terminal == BackendRecipeImportItemTerminal::Cancelled
           || terminal == BackendRecipeImportItemTerminal::Failed;
}

} // namespace

EditInterchangeController::EditInterchangeController(
    DesktopBackend& backend,
    EditController& editor,
    QObject* const parent
) : QObject(parent), backend_(backend), editor_(editor) {
    connect(
        &recipe_adaptation_watcher_,
        &QFutureWatcher<RecipeAdaptationResult>::finished,
        this,
        &EditInterchangeController::finishRecipeAdaptation
    );
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
    connect(&editor_, &EditController::sourceIdentityChanged, this, [this] {
        clearShadowRecipe();
    });
    connect(
        &editor_,
        &EditController::editBaseCommitIdChanged,
        this,
        &EditInterchangeController::recipePreviewChanged
    );
}

EditInterchangeController::~EditInterchangeController() {
    cancelShadowRecipeAdaptation();
    recipe_adaptation_watcher_.waitForFinished();
    closeRecipePlan();
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
    return recipe_ready_ && editor_.active() && !editor_.stateBusy() && recipeTargetIsCurrent()
           && !recipeAdaptationRunning() && recipe_import_plan_.items.isEmpty();
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

int EditInterchangeController::recipeSemanticItemCount() const noexcept {
    return static_cast<int>(recipe_import_plan_.items.size());
}

int EditInterchangeController::recipeSemanticCompletedCount() const noexcept {
    int count = 0;
    for (const auto& item : recipe_import_plan_.items) {
        if (item.terminal == BackendRecipeImportItemTerminal::Completed)
            ++count;
    }
    return count;
}

int EditInterchangeController::recipeSemanticFailedCount() const noexcept {
    int count = 0;
    for (const auto& item : recipe_import_plan_.items) {
        if (recipe_item_failed(item.terminal))
            ++count;
    }
    return count;
}

QVariantList EditInterchangeController::recipeSemanticItems() const {
    QVariantList values;
    values.reserve(recipe_import_plan_.items.size());
    for (const auto& item : recipe_import_plan_.items) {
        QString node_label;
        for (const auto& node : recipe_import_plan_.nodes) {
            if (node.grade_node_id == item.grade_node_id) {
                node_label = node.label;
                break;
            }
        }
        QString detail = item.detail;
        if (item.terminal == BackendRecipeImportItemTerminal::NotFound) {
            detail = tr("No matching subject was found in this photo.");
        } else if (item.terminal == BackendRecipeImportItemTerminal::Unavailable) {
            detail = tr("Check Infer Runtime and the local models, then retry.");
        }
        values.push_back(
            QVariantMap{
                {QStringLiteral("itemId"), item.item_id},
                {QStringLiteral("nodeLabel"), node_label},
                {QStringLiteral("query"), item.semantic_query},
                {QStringLiteral("state"), recipe_item_state(item.terminal)},
                {QStringLiteral("errorText"), detail},
            }
        );
    }
    return values;
}

bool EditInterchangeController::recipeAdaptationRunning() const noexcept {
    return recipe_adaptation_watcher_.isRunning();
}

bool EditInterchangeController::recipeAdaptationPartial() const noexcept {
    return recipeSemanticCompletedCount() > 0 && recipeSemanticFailedCount() > 0;
}

bool EditInterchangeController::recipeCanAdapt() const noexcept {
    return recipe_ready_ && recipeSemanticItemCount() > 0 && !recipeAdaptationRunning()
           && recipeTargetIsCurrent() && recipeSemanticCompletedCount() == 0;
}

bool EditInterchangeController::recipeCanRetryFailed() const noexcept {
    return recipe_ready_ && recipeSemanticFailedCount() > 0 && !recipeAdaptationRunning()
           && recipeTargetIsCurrent();
}

bool EditInterchangeController::recipeCanApplyAvailableNodes() const noexcept {
    return recipe_ready_ && recipeAdaptationPartial() && !recipeAdaptationRunning()
           && recipeTargetIsCurrent();
}

QVariantList EditInterchangeController::recipeWarnings() const {
    QVariantList warnings;
    if (recipe_preview_.managed_mask_node_count > 0) {
        warnings.push_back(tr("%1 managed-mask Grade Nodes will be imported disabled. Their "
                              "source-photo pixels are never reused.")
                               .arg(recipe_preview_.managed_mask_node_count));
    }
    if (recipe_preview_.semantic_mask_intent_count > 0) {
        warnings.push_back(
            tr("%1 semantic managed masks will be omitted; their Grade Nodes remain disabled.")
                .arg(recipe_preview_.semantic_mask_intent_count)
        );
    }
    if (recipe_preview_.removed_lut_count > 0) {
        warnings.push_back(tr("%1 path-bound LUT references will be omitted because this file does "
                              "not contain LUT resources.")
                               .arg(recipe_preview_.removed_lut_count));
    }
    if (recipe_preview_.detached_shared_node_count > 0) {
        warnings.push_back(tr("%1 shared Grade Node links will become independent local nodes.")
                               .arg(recipe_preview_.detached_shared_node_count));
    }
    if (recipe_preview_.foundation_omitted || recipe_preview_.raw_denoise_omitted) {
        warnings.push_back(
            tr("Source development and AI RAW Denoise remain unchanged on this photo.")
        );
    }
    if (recipe_preview_.excluded_retouch_region_count > 0) {
        warnings.push_back(tr("%1 source-photo repair regions will not be imported.")
                               .arg(recipe_preview_.excluded_retouch_region_count));
    }
    if (recipe_preview_.excluded_completion_region_count > 0) {
        warnings.push_back(tr("%1 source-photo AI completion regions will not be imported.")
                               .arg(recipe_preview_.excluded_completion_region_count));
    }
    if (recipe_preview_.excluded_liquify_stroke_count > 0) {
        warnings.push_back(tr("%1 source-photo Liquify strokes will not be imported.")
                               .arg(recipe_preview_.excluded_liquify_stroke_count));
    }
    if (recipe_preview_.canvas_omitted) {
        warnings.push_back(
            tr("Crop, orientation, and perspective remain unchanged on this photo.")
        );
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

QStringList EditInterchangeController::compatibilityWarnings() const {
    QStringList warnings;
    if (preview_.already_applied)
        warnings.append(
            tr("This XMP marks its adjustments as already applied. Importing it may apply the "
               "effects twice.")
        );
    if (!preview_.process_version.isEmpty())
        warnings.append(
            tr("Camera Raw process versions are not replayed exactly; this import uses Shadow's "
               "approximate parameter mapping.")
        );
    return warnings;
}

void EditInterchangeController::clear() {
    xmp_digest_.clear();
    source_name_.clear();
    preview_ = {};
    preview_error_.clear();
    ready_ = false;
    setApplyError({});
    emit previewChanged();
}

void EditInterchangeController::previewXmp(const QUrl& file_url) {
    xmp_digest_.clear();
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
    const QByteArray document = file.read(MAXIMUM_XMP_BYTES + 1);
    if (document.size() > MAXIMUM_XMP_BYTES) {
        preview_error_ = tr("The XMP file is larger than 4 MB.");
        emit previewChanged();
        return;
    }
    xmp_digest_ = QCryptographicHash::hash(document, QCryptographicHash::Sha256).toHex();
    preview_ = parseXmpDevelopImport(document);
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

    const QString import_key =
        editor_.photoId() + QLatin1Char(':') + QString::fromLatin1(xmp_digest_);
    const QString existing_node = applied_xmp_nodes_.value(import_key);
    const auto& current_nodes = editor_.gradeStackForInterchange().grade_nodes;
    if (!existing_node.isEmpty()
        && std::any_of(
            current_nodes.cbegin(),
            current_nodes.cend(),
            [&existing_node](const auto& node) { return node.grade_node_id == existing_node; }
        )) {
        setApplyError(
            tr("This XMP is already imported into the current photo. Undo or remove its imported "
               "node before importing it again.")
        );
        return false;
    }
    QString error;
    if (!editor_.applyXmpDevelopImport(preview_, source_name_, &error)) {
        setApplyError(error);
        return false;
    }
    if (applied_xmp_nodes_.size() >= 128)
        applied_xmp_nodes_.erase(applied_xmp_nodes_.begin());
    applied_xmp_nodes_.insert(import_key, editor_.selectedGradeNodeId());
    emit xmpApplied();
    return true;
}

void EditInterchangeController::clearShadowRecipe() {
    cancelShadowRecipeAdaptation();
    recipe_adaptation_watcher_.waitForFinished();
    closeRecipePlan();
    recipe_source_name_.clear();
    recipe_preview_ = {};
    recipe_preview_error_.clear();
    recipe_target_photo_id_.clear();
    recipe_target_source_path_.clear();
    recipe_target_base_commit_id_.clear();
    recipe_target_working_commit_id_.clear();
    recipe_target_grade_stack_ = {};
    recipe_import_plan_ = {};
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
        recipe_target_source_path_ = editor_.sourcePath();
        recipe_target_base_commit_id_ = editor_.editBaseCommitId();
        recipe_target_working_commit_id_ = editor_.durableWorkingCommitId();
        recipe_target_grade_stack_ = editor_.gradeStackForInterchange();
        file.seek(0);
        recipe_import_plan_ = backend_.prepareSemanticRecipeImport(
            recipe_target_photo_id_,
            recipe_target_source_path_,
            recipe_target_base_commit_id_,
            recipe_target_working_commit_id_,
            recipe_target_grade_stack_,
            file.readAll()
        );
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
    if (!recipe_import_plan_.items.isEmpty()) {
        setRecipeApplyError(tr("Adapt the semantic masks before importing this Shadow Recipe."));
        return false;
    }
    if (!recipeTargetIsCurrent()) {
        setRecipeApplyError(
            tr("The open photo changed after this Recipe was previewed. Preview it again.")
        );
        return false;
    }
    QString error;
    if (!editor_.applyShadowRecipeGradeNodes(recipe_preview_.portable_grade_stack, &error)) {
        setRecipeApplyError(std::move(error));
        return false;
    }
    closeRecipePlan();
    emit shadowRecipeApplied();
    return true;
}

bool EditInterchangeController::startShadowRecipeAdaptation() {
    return startRecipeAdaptation(RecipeAdaptationMode::All);
}

void EditInterchangeController::cancelShadowRecipeAdaptation() {
    if (!recipe_adaptation_watcher_.isRunning())
        return;
    ++recipe_controller_generation_;
    QString item_id;
    std::uint64_t job_token = 0;
    {
        const std::scoped_lock lock(recipe_job_mutex_);
        item_id = recipe_active_item_id_;
        job_token = recipe_active_job_token_;
    }
    if (job_token != 0 && recipe_import_plan_.plan_token != 0) {
        try {
            const auto cancelled = backend_.cancelSemanticRecipeImportItem(
                recipe_import_plan_.plan_token,
                item_id,
                job_token
            );
            Q_UNUSED(cancelled);
        } catch (...) {}
    }
    for (auto& item : recipe_import_plan_.items) {
        if (item.item_id == item_id) {
            item.terminal = BackendRecipeImportItemTerminal::Cancelled;
            item.detail.clear();
            break;
        }
    }
    emit recipePreviewChanged();
}

bool EditInterchangeController::retryFailedShadowRecipeItems() {
    return startRecipeAdaptation(RecipeAdaptationMode::RetryFailed);
}

bool EditInterchangeController::applyAvailableShadowRecipeNodes() {
    return startRecipeAdaptation(RecipeAdaptationMode::ApplyAvailable);
}

bool EditInterchangeController::startRecipeAdaptation(const RecipeAdaptationMode mode) {
    setRecipeApplyError({});
    if (!recipe_ready_ || recipe_adaptation_watcher_.isRunning() || !recipeTargetIsCurrent()) {
        setRecipeApplyError(tr("Preview this Shadow Recipe again before adapting it."));
        return false;
    }
    if (recipe_import_plan_.items.isEmpty()) {
        setRecipeApplyError(tr("This Shadow Recipe does not contain a semantic mask to adapt."));
        return false;
    }
    const auto controller_generation = ++recipe_controller_generation_;
    const auto plan_token = recipe_import_plan_.plan_token;
    const auto photo_id = recipe_target_photo_id_;
    const auto source_path = recipe_target_source_path_;
    const auto base_commit_id = recipe_target_base_commit_id_;
    const auto working_commit_id = recipe_target_working_commit_id_;
    const auto grade_stack = recipe_target_grade_stack_;
    auto initial_plan = recipe_import_plan_;
    recipe_adaptation_watcher_.setFuture(
        QtConcurrent::run([this,
                           mode,
                           controller_generation,
                           plan_token,
                           photo_id,
                           source_path,
                           base_commit_id,
                           working_commit_id,
                           grade_stack,
                           initial_plan]() mutable {
            RecipeAdaptationResult result{
                .plan = std::move(initial_plan),
                .controller_generation = controller_generation,
            };
            const auto publish_plan = [this, controller_generation](BackendRecipeImportPlan plan) {
                QMetaObject::invokeMethod(
                    this,
                    [this, controller_generation, plan = std::move(plan)]() mutable {
                        if (recipe_controller_generation_.load() != controller_generation)
                            return;
                        recipe_import_plan_ = std::move(plan);
                        emit recipePreviewChanged();
                    },
                    Qt::QueuedConnection
                );
            };
            try {
                if (mode == RecipeAdaptationMode::ApplyAvailable) {
                    QSet<QString> failed_nodes;
                    for (const auto& item : result.plan.items) {
                        if (recipe_item_failed(item.terminal))
                            failed_nodes.insert(item.grade_node_id);
                    }
                    for (const auto& node_id : failed_nodes) {
                        result.plan = backend_.excludeSemanticRecipeImportNode(plan_token, node_id);
                    }
                } else {
                    for (const auto& item : result.plan.items) {
                        if (recipe_controller_generation_.load() != controller_generation)
                            break;
                        const bool should_run =
                            mode == RecipeAdaptationMode::All
                                ? item.terminal != BackendRecipeImportItemTerminal::Completed
                                : recipe_item_failed(item.terminal);
                        if (!should_run)
                            continue;
                        const auto job =
                            backend_.beginSemanticRecipeImportItem(plan_token, item.item_id);
                        {
                            const std::scoped_lock lock(recipe_job_mutex_);
                            recipe_active_item_id_ = item.item_id;
                            recipe_active_job_token_ = job;
                        }
                        publish_plan(backend_.semanticRecipeImportPlan(plan_token));
                        auto executed =
                            backend_.executeSemanticRecipeImportItem(plan_token, item.item_id, job);
                        {
                            const std::scoped_lock lock(recipe_job_mutex_);
                            recipe_active_item_id_.clear();
                            recipe_active_job_token_ = 0;
                        }
                        if (executed.terminal == BackendRecipeImportItemTerminal::Staged) {
                            const auto accepted = backend_.acceptSemanticRecipeImportItem(
                                plan_token,
                                item.item_id,
                                executed.proposal_token,
                                executed.generation
                            );
                            Q_UNUSED(accepted);
                        }
                        result.plan = backend_.semanticRecipeImportPlan(plan_token);
                        publish_plan(result.plan);
                    }
                }
                if (recipe_controller_generation_.load() != controller_generation)
                    return result;
                const bool has_failure = std::any_of(
                    result.plan.items.cbegin(),
                    result.plan.items.cend(),
                    [](const auto& item) { return recipe_item_failed(item.terminal); }
                );
                if (!has_failure || mode == RecipeAdaptationMode::ApplyAvailable) {
                    result.grade_stack = backend_.finalizeSemanticRecipeImport(
                        plan_token,
                        photo_id,
                        source_path,
                        base_commit_id,
                        working_commit_id,
                        grade_stack
                    );
                    result.finalized = true;
                }
            } catch (const std::exception& error) {
                result.error = QString::fromUtf8(error.what());
                try {
                    result.plan = backend_.semanticRecipeImportPlan(plan_token);
                } catch (...) {}
            }
            return result;
        })
    );
    emit recipePreviewChanged();
    return true;
}

void EditInterchangeController::finishRecipeAdaptation() {
    const auto result = recipe_adaptation_watcher_.result();
    if (result.controller_generation != recipe_controller_generation_.load()
        || !recipeTargetIsCurrent()) {
        emit recipePreviewChanged();
        return;
    }
    recipe_import_plan_ = result.plan;
    if (!result.error.isEmpty()) {
        setRecipeApplyError(tr("Shadow could not adapt this Recipe: %1").arg(result.error));
    }
    if (result.finalized) {
        QString error;
        if (editor_.applyShadowRecipeGradeNodes(result.grade_stack, &error)) {
            closeRecipePlan();
            emit shadowRecipeApplied();
        } else {
            setRecipeApplyError(std::move(error));
        }
    }
    emit recipePreviewChanged();
}

bool EditInterchangeController::recipeTargetIsCurrent() const noexcept {
    return editor_.photoId() == recipe_target_photo_id_
           && editor_.sourcePath() == recipe_target_source_path_
           && editor_.editBaseCommitId() == recipe_target_base_commit_id_
           && editor_.durableWorkingCommitId() == recipe_target_working_commit_id_
           && editor_.gradeStackForInterchange() == recipe_target_grade_stack_;
}

void EditInterchangeController::closeRecipePlan() noexcept {
    if (recipe_import_plan_.plan_token == 0)
        return;
    try {
        backend_.closeSemanticRecipeImport(recipe_import_plan_.plan_token);
    } catch (...) {}
    recipe_import_plan_.plan_token = 0;
}

bool EditInterchangeController::exportShadowRecipe(const QUrl& file_url, const QString& label) {
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
