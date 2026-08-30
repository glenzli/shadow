#include <QFile>
#include <QString>

#include <cstdlib>
#include <iostream>

namespace {

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "Edit interchange controller contract failed: " << message << '\n';
    }
    return condition;
}

[[nodiscard]] QString source(const char* const relative_path) {
    QFile file(QStringLiteral(SHADOW_DESKTOP_SOURCE_DIR) + QString::fromUtf8(relative_path));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(file.readAll());
}

[[nodiscard]] bool backendProjectionContract() {
    const auto backend = source("/src/desktop_backend_edit.cpp");
    return require(!backend.isEmpty(), "backend projection source is readable")
           && require(
               backend.contains("prepare_semantic_recipe_import"),
               "prepare projects the stable Rust session method"
           )
           && require(
               backend.contains("execute_semantic_recipe_import_item")
                   && backend.contains("cancel_semantic_recipe_import_item")
                   && backend.contains("accept_semantic_recipe_import_item"),
               "execute, cancel, and accept remain explicit session projections"
           )
           && require(
               backend.contains("finalize_semantic_recipe_import")
                   && backend.contains("close_semantic_recipe_import"),
               "finalize and close preserve separate authority boundaries"
           )
           && require(
               backend.contains("unsupported semantic Recipe import terminal"),
               "unknown Rust terminal values fail closed"
           );
}

[[nodiscard]] bool asynchronousControllerContract() {
    const auto controller = source("/src/edit_interchange_controller.cpp");
    if (!require(!controller.isEmpty(), "controller source is readable"))
        return false;
    const auto cancel_start =
        controller.indexOf("void EditInterchangeController::cancelShadowRecipeAdaptation()");
    const auto invalidate = controller.indexOf("++recipe_controller_generation_", cancel_start);
    const auto backend_cancel = controller.indexOf("cancelSemanticRecipeImportItem", cancel_start);
    const auto worker = controller.indexOf("QtConcurrent::run(");
    const auto begin = controller.indexOf("beginSemanticRecipeImportItem", worker);
    const auto execute = controller.indexOf("executeSemanticRecipeImportItem", begin);
    const auto accept = controller.indexOf("acceptSemanticRecipeImportItem", execute);
    return require(worker >= 0, "semantic adaptation runs outside the UI thread")
           && require(
               begin > worker && execute > begin && accept > execute,
               "each item is prepared, executed, and accepted serially"
           )
           && require(
               invalidate > cancel_start && backend_cancel > invalidate,
               "cancel invalidates the local generation before backend cancellation"
           )
           && require(
               controller.contains("result.controller_generation != recipe_controller_generation_"),
               "late worker results are generation suppressed"
           )
           && require(
               controller.contains(
                   "editor_.gradeStackForInterchange() == recipe_target_grade_stack_"
               ),
               "apply rechecks the complete frozen grade stack"
           )
           && require(
               controller.contains("recipe_adaptation_watcher_.waitForFinished()"),
               "destruction and plan replacement wait for worker retirement"
           );
}

[[nodiscard]] bool qmlSurfaceContract() {
    const auto header = source("/src/edit_interchange_controller.hpp");
    return require(
               header.contains("recipeSemanticItems") && header.contains("recipeAdaptationRunning")
                   && header.contains("recipeCanApplyAvailableNodes"),
               "the production controller exposes the semantic Recipe QML properties"
           )
           && require(
               header.contains("startShadowRecipeAdaptation")
                   && header.contains("retryFailedShadowRecipeItems")
                   && header.contains("applyAvailableShadowRecipeNodes"),
               "the production controller exposes adapt, retry, and available-node actions"
           );
}

} // namespace

int main() {
    return backendProjectionContract() && asynchronousControllerContract() && qmlSurfaceContract()
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}
