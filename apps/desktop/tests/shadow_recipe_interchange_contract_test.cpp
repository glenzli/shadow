#include <QFile>
#include <QString>

#include <cstdlib>
#include <iostream>

namespace {

[[nodiscard]] QString readSource(const QString& relative_path) {
    QFile file(QStringLiteral(SHADOW_DESKTOP_SOURCE_DIR "/") + relative_path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "Shadow Recipe interchange contract failed: " << message << '\n';
    }
    return condition;
}

} // namespace

int main() {
    const QString dialog = readSource(QStringLiteral("qml/ShadowRecipeInterchangeDialog.qml"));
    const QString menus = readSource(QStringLiteral("qml/PrecisionGradeNodeMenus.qml"));
    const QString controller = readSource(QStringLiteral("src/edit_interchange_controller.cpp"));
    const QString application = readSource(QStringLiteral("src/edit_interchange_application.cpp"));

    if (!require(!dialog.isEmpty(), "the packaged Recipe dialog source is readable")
        || !require(
            dialog.contains(QStringLiteral("recipeCanApply"))
                && dialog.contains(QStringLiteral("Replace Grade Nodes"))
                && dialog.contains(QStringLiteral("recipeWarnings")),
            "the import surface exposes validated preview, compatibility notes, and explicit replacement"
        )
        || !require(
            dialog.contains(QStringLiteral("parent.height - 48"))
                && dialog.contains(QStringLiteral("ScrollView")),
            "the modal remains operable inside a short host window"
        )
        || !require(
            dialog.contains(QStringLiteral("FileDialog.OpenFile"))
                && dialog.contains(QStringLiteral("FileDialog.SaveFile"))
                && dialog.contains(QStringLiteral("defaultSuffix: \"shadowrecipe\"")),
            "one surface owns both versioned Recipe file directions"
        )
        || !require(
            menus.contains(QStringLiteral("Import Shadow Recipe…"))
                && menus.contains(QStringLiteral("Export current Recipe…"))
                && menus.contains(QStringLiteral("ShadowRecipeInterchangeDialog")),
            "the Grade Node menu exposes import and export without a second edit subsystem"
        )
        || !require(
            controller.contains(QStringLiteral("QSaveFile"))
                && controller.contains(QStringLiteral("file.commit()"))
                && controller.contains(QStringLiteral("recipe_target_photo_id_")),
            "publication is atomic and preview application is bound to its photo"
        )
        || !require(
            application.contains(QStringLiteral("imported.grade_nodes = portable_grade_stack.grade_nodes"))
                && application.contains(QStringLiteral("recordWorkingTransition"))
                && application.contains(QStringLiteral("schedulePreview(0)")),
            "one undoable transaction replaces only Grade Nodes and schedules the canonical preview"
        )) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
