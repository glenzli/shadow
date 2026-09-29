#include <QCoreApplication>
#include <QGuiApplication>
#include <QMetaObject>
#include <QObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QString>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <utility>

class FakeRecipeEditor final : public QObject {
    Q_OBJECT
};

class FakeRecipeInterchangeController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString recipeSourceName READ recipeSourceName NOTIFY stateChanged)
    Q_PROPERTY(bool recipeReady READ recipeReady NOTIFY stateChanged)
    Q_PROPERTY(QString recipeLabel READ recipeLabel NOTIFY stateChanged)
    Q_PROPERTY(int recipeGradeNodeCount READ recipeGradeNodeCount NOTIFY stateChanged)
    Q_PROPERTY(int recipePortableMaskCount READ recipePortableMaskCount NOTIFY stateChanged)
    Q_PROPERTY(QString recipeErrorText READ recipeErrorText NOTIFY stateChanged)
    Q_PROPERTY(QVariantList recipeWarnings READ recipeWarnings NOTIFY stateChanged)
    Q_PROPERTY(QString recipeApplyErrorText READ recipeApplyErrorText NOTIFY stateChanged)
    Q_PROPERTY(QString recipeExportErrorText READ recipeExportErrorText NOTIFY stateChanged)
    Q_PROPERTY(bool recipeCanApply READ recipeCanApply NOTIFY stateChanged)
    Q_PROPERTY(int recipeSemanticItemCount READ recipeSemanticItemCount NOTIFY stateChanged)
    Q_PROPERTY(
        int recipeSemanticCompletedCount READ recipeSemanticCompletedCount NOTIFY stateChanged
    )
    Q_PROPERTY(int recipeSemanticFailedCount READ recipeSemanticFailedCount NOTIFY stateChanged)
    Q_PROPERTY(QVariantList recipeSemanticItems READ recipeSemanticItems NOTIFY stateChanged)
    Q_PROPERTY(bool recipeAdaptationRunning READ recipeAdaptationRunning NOTIFY stateChanged)
    Q_PROPERTY(bool recipeAdaptationPartial READ recipeAdaptationPartial NOTIFY stateChanged)
    Q_PROPERTY(bool recipeCanAdapt READ recipeCanAdapt NOTIFY stateChanged)
    Q_PROPERTY(bool recipeCanRetryFailed READ recipeCanRetryFailed NOTIFY stateChanged)
    Q_PROPERTY(
        bool recipeCanApplyAvailableNodes READ recipeCanApplyAvailableNodes NOTIFY stateChanged
    )

  public:
    using QObject::QObject;

    [[nodiscard]] QString recipeSourceName() const {
        return QStringLiteral("Portrait.shadowrecipe");
    }
    [[nodiscard]] bool recipeReady() const noexcept {
        return true;
    }
    [[nodiscard]] QString recipeLabel() const {
        return QStringLiteral("Portrait");
    }
    [[nodiscard]] int recipeGradeNodeCount() const noexcept {
        return 3;
    }
    [[nodiscard]] int recipePortableMaskCount() const noexcept {
        return 2;
    }
    [[nodiscard]] QString recipeErrorText() const {
        return {};
    }
    [[nodiscard]] QVariantList recipeWarnings() const {
        return {};
    }
    [[nodiscard]] QString recipeApplyErrorText() const {
        return {};
    }
    [[nodiscard]] QString recipeExportErrorText() const {
        return {};
    }
    [[nodiscard]] bool recipeCanApply() const noexcept {
        return can_apply_;
    }
    [[nodiscard]] int recipeSemanticItemCount() const noexcept {
        return static_cast<int>(items_.size());
    }
    [[nodiscard]] int recipeSemanticCompletedCount() const noexcept {
        return completed_count_;
    }
    [[nodiscard]] int recipeSemanticFailedCount() const noexcept {
        return failed_count_;
    }
    [[nodiscard]] QVariantList recipeSemanticItems() const {
        return items_;
    }
    [[nodiscard]] bool recipeAdaptationRunning() const noexcept {
        return running_;
    }
    [[nodiscard]] bool recipeAdaptationPartial() const noexcept {
        return partial_;
    }
    [[nodiscard]] bool recipeCanAdapt() const noexcept {
        return can_adapt_;
    }
    [[nodiscard]] bool recipeCanRetryFailed() const noexcept {
        return can_retry_;
    }
    [[nodiscard]] bool recipeCanApplyAvailableNodes() const noexcept {
        return can_apply_available_;
    }

    void setScenario(
        QVariantList items,
        const int completed_count,
        const int failed_count,
        const bool running,
        const bool partial,
        const bool can_adapt,
        const bool can_retry,
        const bool can_apply_available,
        const bool can_apply
    ) {
        items_ = std::move(items);
        completed_count_ = completed_count;
        failed_count_ = failed_count;
        running_ = running;
        partial_ = partial;
        can_adapt_ = can_adapt;
        can_retry_ = can_retry;
        can_apply_available_ = can_apply_available;
        can_apply_ = can_apply;
        emit stateChanged();
    }

    Q_INVOKABLE void clearShadowRecipe() {}
    Q_INVOKABLE bool applyShadowRecipe() {
        ++apply_count_;
        return true;
    }
    Q_INVOKABLE bool startShadowRecipeAdaptation() {
        ++start_count_;
        return true;
    }
    Q_INVOKABLE void cancelShadowRecipeAdaptation() {
        ++cancel_count_;
    }
    Q_INVOKABLE bool retryFailedShadowRecipeItems() {
        ++retry_count_;
        return true;
    }
    Q_INVOKABLE bool applyAvailableShadowRecipeNodes() {
        ++apply_available_count_;
        return true;
    }

    int apply_count_ = 0;
    int start_count_ = 0;
    int cancel_count_ = 0;
    int retry_count_ = 0;
    int apply_available_count_ = 0;

  signals:
    void stateChanged();

  private:
    QVariantList items_;
    int completed_count_ = 0;
    int failed_count_ = 0;
    bool running_ = false;
    bool partial_ = false;
    bool can_adapt_ = false;
    bool can_retry_ = false;
    bool can_apply_available_ = false;
    bool can_apply_ = true;
};

namespace {

[[nodiscard]] QVariantMap semanticItem(
    const QString& id,
    const QString& label,
    const QString& query,
    const QString& state,
    const QString& error = {}
) {
    return {
        {QStringLiteral("itemId"), id},
        {QStringLiteral("nodeLabel"), label},
        {QStringLiteral("query"), query},
        {QStringLiteral("state"), state},
        {QStringLiteral("errorText"), error},
    };
}

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "Shadow Recipe interchange contract failed: " << message << '\n';
    }
    return condition;
}

void drainBindings() {
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}

[[nodiscard]] bool invokeBool(QObject* target, const char* method) {
    QVariant result;
    return QMetaObject::invokeMethod(target, method, Q_RETURN_ARG(QVariant, result))
           && result.toBool();
}

[[nodiscard]] bool invokeVoid(QObject* target, const char* method) {
    return QMetaObject::invokeMethod(target, method);
}

[[nodiscard]] QString invokeItemDetail(QObject* target, const QVariantMap& item) {
    QVariant result;
    if (!QMetaObject::invokeMethod(
            target,
            "semanticItemDetail",
            Q_RETURN_ARG(QVariant, result),
            Q_ARG(QVariant, item)
        )) {
        return {};
    }
    return result.toString();
}

[[nodiscard]] QString invokeStateLabel(QObject* target, const QString& state) {
    QVariant result;
    if (!QMetaObject::invokeMethod(
            target,
            "semanticStateLabel",
            Q_RETURN_ARG(QVariant, result),
            Q_ARG(QVariant, state)
        )) {
        return {};
    }
    return result.toString();
}

} // namespace

int main(int argc, char* argv[]) {
    QGuiApplication application(argc, argv);
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.loadFromModule(
        QStringLiteral("Shadow.RecipeInterchangeContract"),
        QStringLiteral("ShadowRecipeInterchangeDialog")
    );
    FakeRecipeEditor editor;
    FakeRecipeInterchangeController controller;
    std::unique_ptr<QObject> dialog(component.createWithInitialProperties({
        {QStringLiteral("editor"), QVariant::fromValue(&editor)},
        {QStringLiteral("interchangeController"), QVariant::fromValue(&controller)},
    }));
    if (!dialog) {
        std::cerr << component.errorString().toStdString();
        return EXIT_FAILURE;
    }

    QQuickWindow window;
    window.resize(520, 380);
    dialog->setProperty("parent", QVariant::fromValue(window.contentItem()));
    if (!invokeVoid(dialog.get(), "open")) {
        return EXIT_FAILURE;
    }
    drainBindings();

    QQuickItem* const primary_button =
        dialog->findChild<QQuickItem*>(QStringLiteral("applyShadowRecipeButton"));
    QQuickItem* const cancel_button =
        dialog->findChild<QQuickItem*>(QStringLiteral("cancelShadowRecipeButton"));
    QQuickItem* const retry_button =
        dialog->findChild<QQuickItem*>(QStringLiteral("retryFailedShadowRecipeButton"));
    QQuickItem* const available_button =
        dialog->findChild<QQuickItem*>(QStringLiteral("applyAvailableShadowRecipeButton"));
    QQuickItem* const footer = dialog->findChild<QQuickItem*>(QStringLiteral("recipeDialogFooter"));
    if (!require(
            primary_button != nullptr && cancel_button != nullptr && retry_button != nullptr
                && available_button != nullptr && footer != nullptr,
            "the stable bottom action surface is materialized"
        )
        || !require(
            primary_button->property("text").toString() == QStringLiteral("Replace Grade Nodes")
                && primary_button->isEnabled(),
            "a Recipe without semantic masks keeps the existing one-step import"
        )) {
        return EXIT_FAILURE;
    }

    const QVariantMap waiting = semanticItem(
        QStringLiteral("mask-1"),
        QStringLiteral("Subject"),
        QStringLiteral("portrait subject"),
        QStringLiteral("waiting")
    );
    controller.setScenario({waiting}, 0, 0, false, false, true, false, false, false);
    drainBindings();
    if (!require(
            primary_button->property("text").toString() == QStringLiteral("Adapt and Import")
                && primary_button->isEnabled() && invokeVoid(primary_button, "clicked")
                && controller.start_count_ == 1,
            "the primary control starts one semantic adaptation interaction"
        )) {
        return EXIT_FAILURE;
    }

    controller.setScenario({waiting}, 0, 0, true, false, false, false, false, false);
    drainBindings();
    if (!require(
            !primary_button->isEnabled()
                && cancel_button->property("text").toString() == QStringLiteral("Cancel adaptation")
                && invokeVoid(cancel_button, "clicked") && controller.cancel_count_ == 1,
            "the running surface keeps one real cancellation interaction visible"
        )) {
        return EXIT_FAILURE;
    }

    const QVariantMap completed = semanticItem(
        QStringLiteral("mask-1"),
        QStringLiteral("Subject"),
        QStringLiteral("portrait subject"),
        QStringLiteral("completed")
    );
    const QVariantMap completed_two = semanticItem(
        QStringLiteral("mask-2"),
        QStringLiteral("Background"),
        QStringLiteral("sky"),
        QStringLiteral("completed")
    );
    controller
        .setScenario({completed, completed_two}, 2, 0, false, false, false, false, false, true);
    drainBindings();
    if (!require(
            dialog->property("semanticProgress").toDouble() == 1.0
                && primary_button->property("text").toString()
                       == QStringLiteral("Import adapted Recipe")
                && invokeBool(dialog.get(), "applyRecipe") && controller.apply_count_ == 1,
            "a fully adapted Recipe uses the existing atomic apply path"
        )) {
        return EXIT_FAILURE;
    }

    const QVariantMap failed = semanticItem(
        QStringLiteral("mask-2"),
        QStringLiteral("Background"),
        QStringLiteral("sky"),
        QStringLiteral("failed"),
        QStringLiteral("segmentation failed")
    );
    controller.setScenario({completed, failed}, 1, 1, false, true, false, true, true, false);
    drainBindings();
    if (!require(
            !primary_button->property("visible").toBool()
                && retry_button->property("visible").toBool()
                && available_button->property("visible").toBool()
                && !invokeBool(dialog.get(), "applyRecipe") && controller.apply_count_ == 1,
            "partial adaptation disables the ordinary all-node apply path"
        )
        || !require(
            invokeBool(dialog.get(), "retryFailedItems") && controller.retry_count_ == 1
                && invokeBool(dialog.get(), "applyAvailableNodes")
                && controller.apply_available_count_ == 1,
            "partial adaptation exposes retry and explicit whole-node exclusion"
        )) {
        return EXIT_FAILURE;
    }

    const QVariantMap unavailable = semanticItem(
        QStringLiteral("mask-2"),
        QStringLiteral("Background"),
        QStringLiteral("sky"),
        QStringLiteral("unavailable"),
        QStringLiteral("private runtime diagnostic must not be presented")
    );
    const QString unavailable_detail = invokeItemDetail(dialog.get(), unavailable);
    if (!require(
            unavailable_detail.contains(QStringLiteral("Infer Runtime"))
                && unavailable_detail.contains(QStringLiteral("local models"))
                && !unavailable_detail.contains(QStringLiteral("private runtime")),
            "unavailable guidance is bounded to Infer Runtime and local-model readiness"
        )) {
        return EXIT_FAILURE;
    }

    controller.setScenario(
        {semanticItem(
            QStringLiteral("mask-1"),
            QStringLiteral("Subject"),
            QStringLiteral("portrait subject"),
            QStringLiteral("cancelled")
        )},
        0,
        1,
        false,
        true,
        false,
        true,
        false,
        false
    );
    drainBindings();
    if (!require(
            invokeStateLabel(dialog.get(), QStringLiteral("cancelled"))
                == QStringLiteral("Cancelled"),
            "cancelled semantic items retain an explicit terminal presentation"
        )) {
        return EXIT_FAILURE;
    }

    dialog->setProperty("height", 300);
    drainBindings();
    if (!require(
            footer->isVisible() && footer->y() >= 0
                && footer->y() + footer->height() <= dialog->property("height").toDouble(),
            "resizing keeps the footer and all recovery actions inside the dialog"
        )) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

#include "shadow_recipe_interchange_contract_test.moc"
