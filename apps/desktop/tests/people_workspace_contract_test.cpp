#include <QCoreApplication>
#include <QGuiApplication>
#include <QImage>
#include <QMetaObject>
#include <QMouseEvent>
#include <QObject>
#include <QPointingDevice>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSet>
#include <QVariantList>
#include <QVariantMap>

#include <cstdlib>
#include <iostream>
#include <memory>

class FakePeopleAnalysisController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool cancelRequested READ cancelRequested NOTIFY changed)
    Q_PROPERTY(bool hasResults READ hasResults NOTIFY changed)
    Q_PROPERTY(QString statusText READ statusText NOTIFY changed)
    Q_PROPERTY(QString errorText READ errorText NOTIFY changed)
    Q_PROPERTY(QVariantList groups READ groups NOTIFY changed)
    Q_PROPERTY(uint analyzedPhotos READ analyzedPhotos NOTIFY changed)
    Q_PROPERTY(uint detectedFaces READ detectedFaces NOTIFY changed)
    Q_PROPERTY(uint embeddedFaces READ embeddedFaces NOTIFY changed)
    Q_PROPERTY(uint skippedItems READ skippedItems NOTIFY changed)
    Q_PROPERTY(uint ungroupedFaces READ ungroupedFaces NOTIFY changed)
    Q_PROPERTY(bool truncated READ truncated NOTIFY changed)
    Q_PROPERTY(int selectedGroupCount READ selectedGroupCount NOTIFY changed)
    Q_PROPERTY(bool canMergeSelectedGroups READ canMergeSelectedGroups NOTIFY changed)
    Q_PROPERTY(bool canUndoMerge READ canUndoMerge NOTIFY changed)
    Q_PROPERTY(QString mergeSelectionText READ mergeSelectionText NOTIFY changed)

  public:
    bool busy() const noexcept {
        return busy_;
    }
    bool cancelRequested() const noexcept {
        return cancel_requested_;
    }
    bool hasResults() const noexcept {
        return has_results_;
    }
    QString statusText() const {
        return QStringLiteral("Ready");
    }
    QString errorText() const {
        return {};
    }
    QVariantList groups() const {
        if (!has_results_) {
            return {};
        }
        if (merged_) {
            return {
                QVariantMap{
                    {QStringLiteral("groupId"), QStringLiteral("session-merged-person-1")},
                    {QStringLiteral("displayName"), QStringLiteral("Alice")},
                    {QStringLiteral("displayIndex"), 1},
                    {QStringLiteral("photoCount"), 5},
                    {QStringLiteral("thumbnailSource"), QString{}},
                    {QStringLiteral("selected"), false},
                    {QStringLiteral("merged"), true},
                },
            };
        }
        return {
            QVariantMap{
                {QStringLiteral("groupId"), QStringLiteral("a")},
                {QStringLiteral("displayName"), first_name_},
                {QStringLiteral("displayIndex"), 1},
                {QStringLiteral("photoCount"), 3},
                {QStringLiteral("thumbnailSource"), QString{}},
                {QStringLiteral("selected"), selected_.contains(QStringLiteral("a"))},
                {QStringLiteral("merged"), false},
            },
            QVariantMap{
                {QStringLiteral("groupId"), QStringLiteral("b")},
                {QStringLiteral("displayName"), QString{}},
                {QStringLiteral("displayIndex"), 2},
                {QStringLiteral("photoCount"), 2},
                {QStringLiteral("thumbnailSource"), QString{}},
                {QStringLiteral("selected"), selected_.contains(QStringLiteral("b"))},
                {QStringLiteral("merged"), false},
            },
        };
    }
    uint analyzedPhotos() const noexcept {
        return has_results_ ? 9U : 0U;
    }
    uint detectedFaces() const noexcept {
        return has_results_ ? 6U : 0U;
    }
    uint embeddedFaces() const noexcept {
        return has_results_ ? 5U : 0U;
    }
    uint skippedItems() const noexcept {
        return has_results_ ? 1U : 0U;
    }
    uint ungroupedFaces() const noexcept {
        return 0;
    }
    bool truncated() const noexcept {
        return false;
    }
    int selectedGroupCount() const noexcept {
        return static_cast<int>(selected_.size());
    }
    bool canMergeSelectedGroups() const noexcept {
        return selected_.size() >= 2;
    }
    bool canUndoMerge() const noexcept {
        return can_undo_merge_;
    }
    QString mergeSelectionText() const {
        return selected_.size() >= 2 ? QStringLiteral("Ready to merge")
                                     : QStringLiteral("Select people");
    }

    Q_INVOKABLE void startAnalysis() {
        ++start_count;
        has_results_ = true;
        emit changed();
    }
    Q_INVOKABLE void cancelAnalysis() {
        ++cancel_count;
        cancel_requested_ = true;
        busy_ = false;
        emit changed();
    }
    Q_INVOKABLE void clearPeopleData() {
        ++clear_count;
        has_results_ = false;
        selected_.clear();
        emit changed();
    }
    Q_INVOKABLE void toggleGroupSelection(const QString& group_id) {
        if (selected_.contains(group_id)) {
            selected_.remove(group_id);
        } else {
            selected_.insert(group_id);
        }
        emit changed();
    }
    Q_INVOKABLE void mergeSelectedGroups() {
        if (!canMergeSelectedGroups()) {
            return;
        }
        ++merge_count;
        merged_ = true;
        can_undo_merge_ = true;
        selected_.clear();
        emit changed();
    }
    Q_INVOKABLE void renameGroup(const QString& group_id, const QString& display_name) {
        if (group_id != QStringLiteral("a")) {
            return;
        }
        ++rename_count;
        first_name_ = display_name.trimmed();
        emit changed();
    }
    Q_INVOKABLE void openGroup(const QString&) {
        ++open_count;
    }
    Q_INVOKABLE void undoLastMerge() {
        if (!can_undo_merge_) {
            return;
        }
        ++undo_count;
        merged_ = false;
        can_undo_merge_ = false;
        emit changed();
    }

    int open_count = 0;
    int start_count = 0;
    int clear_count = 0;
    int merge_count = 0;
    int undo_count = 0;
    int rename_count = 0;
    int cancel_count = 0;

    void setBusy(const bool busy) {
        busy_ = busy;
        cancel_requested_ = false;
        emit changed();
    }

  signals:
    void changed();

  private:
    bool has_results_ = false;
    bool busy_ = false;
    bool cancel_requested_ = false;
    bool merged_ = false;
    bool can_undo_merge_ = false;
    QString first_name_;
    QSet<QString> selected_;
};

class FakePeoplePreferences final : public QObject {
    Q_OBJECT
    Q_PROPERTY(
        bool peopleAnalysisExecutionAllowed READ peopleAnalysisExecutionAllowed NOTIFY changed
    )
    Q_PROPERTY(bool peopleAnalysisConsentDecided READ peopleAnalysisConsentDecided NOTIFY changed)

  public:
    bool peopleAnalysisExecutionAllowed() const noexcept {
        return allowed_;
    }
    bool peopleAnalysisConsentDecided() const noexcept {
        return decided_;
    }

    Q_INVOKABLE void grantPeopleAnalysisConsent() {
        allowed_ = true;
        decided_ = true;
        emit changed();
    }
    Q_INVOKABLE void denyPeopleAnalysisConsent() {
        allowed_ = false;
        decided_ = true;
        emit changed();
    }
    Q_INVOKABLE void revokePeopleAnalysisConsent() {
        denyPeopleAnalysisConsent();
    }

  signals:
    void changed();

  private:
    bool allowed_ = false;
    bool decided_ = false;
};

namespace {

bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "People workspace contract failed: " << message << '\n';
    }
    return condition;
}

void drainBindings() {
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}

QQuickItem* visualItem(QQuickItem* root, const QString& name) {
    if (root->objectName() == name)
        return root;
    for (auto* child : root->childItems())
        if (auto* found = visualItem(child, name))
            return found;
    return nullptr;
}

void click(QQuickWindow& window, QQuickItem& item) {
    const QPointF position = item.mapToScene(QPointF{item.width() / 2.0, item.height() / 2.0});
    QMouseEvent press{
        QEvent::MouseButtonPress,
        position,
        position,
        position,
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::NoModifier,
        QPointingDevice::primaryPointingDevice(),
    };
    QGuiApplication::sendEvent(&window, &press);
    QMouseEvent release{
        QEvent::MouseButtonRelease,
        position,
        position,
        position,
        Qt::LeftButton,
        Qt::NoButton,
        Qt::NoModifier,
        QPointingDevice::primaryPointingDevice(),
    };
    QGuiApplication::sendEvent(&window, &release);
    drainBindings();
}

} // namespace

int main(int argc, char* argv[]) {
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QGuiApplication application(argc, argv);
    QQmlEngine engine;
    QQmlComponent component{&engine};
    component.loadFromModule(
        QStringLiteral("Shadow.PeopleWorkspaceContract"),
        QStringLiteral("PeopleWorkspace")
    );
    FakePeopleAnalysisController controller;
    FakePeoplePreferences preferences;
    std::unique_ptr<QObject> object{component.createWithInitialProperties({
        {QStringLiteral("controller"), QVariant::fromValue(&controller)},
        {QStringLiteral("aiPreferences"), QVariant::fromValue(&preferences)},
        {QStringLiteral("width"), 900.0},
        {QStringLiteral("height"), 700.0},
    })};
    auto* const workspace = qobject_cast<QQuickItem*>(object.get());
    if (!workspace) {
        std::cerr << component.errorString().toStdString();
        return EXIT_FAILURE;
    }

    QQuickWindow window;
    window.setGeometry(0, 0, 900, 700);
    workspace->setParentItem(window.contentItem());
    window.show();
    drainBindings();

    auto* const start_button =
        workspace->findChild<QQuickItem*>(QStringLiteral("peopleStartButton"));
    if (!require(start_button != nullptr, "manual analysis action is packaged")) {
        return EXIT_FAILURE;
    }

    controller.setBusy(true);
    drainBindings();
    auto* const cancel_button =
        workspace->findChild<QQuickItem*>(QStringLiteral("peopleCancelButton"));
    if (!require(cancel_button != nullptr, "running analysis exposes a stop action")) {
        return EXIT_FAILURE;
    }
    click(window, *cancel_button);
    if (!require(controller.cancel_count == 1, "stop action reaches the controller")) {
        return EXIT_FAILURE;
    }

    click(window, *start_button);
    QObject* const consent_dialog =
        workspace->findChild<QObject*>(QStringLiteral("peopleConsentDialog"));
    if (!require(controller.start_count == 0, "analysis is gated before first-use consent")
        || !require(
            consent_dialog != nullptr && consent_dialog->property("opened").toBool(),
            "first use opens the local people consent dialog"
        )
        || !require(
            QMetaObject::invokeMethod(consent_dialog, "accept"),
            "consent dialog can accept the local processing policy"
        )) {
        return EXIT_FAILURE;
    }
    drainBindings();
    if (!require(controller.start_count == 1, "consent starts the requested analysis")
        || !require(preferences.peopleAnalysisExecutionAllowed(), "consent is retained")
        || !require(
            workspace->property("renderedGroupCount").toInt() == 2,
            "anonymous group cards follow stored people data"
        )) {
        return EXIT_FAILURE;
    }

    if (!require(
            QMetaObject::invokeMethod(
                workspace,
                "requestRenameRenderedGroup",
                Q_ARG(QVariant, QVariant{0})
            ),
            "person naming entry is packaged"
        )) {
        return EXIT_FAILURE;
    }
    drainBindings();
    QObject* const name_dialog = workspace->findChild<QObject*>(QStringLiteral("peopleNameDialog"));
    QObject* const name_field = workspace->findChild<QObject*>(QStringLiteral("peopleNameField"));
    if (!require(
            name_dialog != nullptr && name_dialog->property("opened").toBool(),
            "naming opens a focused dialog"
        )
        || !require(name_field != nullptr, "naming field is packaged")) {
        return EXIT_FAILURE;
    }
    name_field->setProperty("text", QStringLiteral("Alice"));
    if (!require(QMetaObject::invokeMethod(name_dialog, "accept"), "person name can be saved")) {
        return EXIT_FAILURE;
    }
    drainBindings();
    if (!require(controller.rename_count == 1, "saved name reaches the controller")
        || !require(
            controller.groups().front().toMap().value(QStringLiteral("displayName")).toString()
                == QStringLiteral("Alice"),
            "saved name immediately replaces the anonymous fallback"
        )) {
        return EXIT_FAILURE;
    }

    auto* const name_search = workspace->findChild<QObject*>(QStringLiteral("peopleNameSearch"));
    if (!require(name_search != nullptr, "name search is packaged"))
        return EXIT_FAILURE;
    name_search->setProperty("text", QStringLiteral("ali"));
    drainBindings();
    if (!require(
            workspace->property("renderedGroupCount").toInt() == 1,
            "name search filters case-insensitively"
        ))
        return EXIT_FAILURE;
    name_search->setProperty("text", QString{});
    drainBindings();
    auto* const card = visualItem(workspace, QStringLiteral("peopleGroupCard"));
    if (!require(card != nullptr, "visible person card exists"))
        return EXIT_FAILURE;
    click(window, *card);
    if (!require(
            controller.open_count == 1 && controller.selectedGroupCount() == 0,
            "ordinary card click opens photos without selecting a merge"
        ))
        return EXIT_FAILURE;
    workspace->setProperty("mergeMode", true);
    drainBindings();
    if (!require(
            QMetaObject::invokeMethod(
                workspace,
                "requestToggleRenderedGroup",
                Q_ARG(QVariant, QVariant{0})
            ),
            "first person card selection entry is packaged"
        )
        || !require(
            QMetaObject::invokeMethod(
                workspace,
                "requestToggleRenderedGroup",
                Q_ARG(QVariant, QVariant{1})
            ),
            "second person card selection entry is packaged"
        )) {
        return EXIT_FAILURE;
    }
    drainBindings();
    auto* const merge_button =
        workspace->findChild<QQuickItem*>(QStringLiteral("peopleMergeButton"));
    if (!require(controller.selectedGroupCount() == 2, "cards select merge candidates")
        || !require(merge_button != nullptr, "merge action is packaged")) {
        return EXIT_FAILURE;
    }
    click(window, *merge_button);
    if (!require(controller.merge_count == 1, "merge action reaches the controller")
        || !require(
            workspace->property("renderedGroupCount").toInt() == 1,
            "merged people become one visible group"
        )) {
        return EXIT_FAILURE;
    }
    auto* const undo_button =
        workspace->findChild<QQuickItem*>(QStringLiteral("peopleUndoMergeButton"));
    if (!require(undo_button != nullptr, "merge undo is packaged")) {
        return EXIT_FAILURE;
    }
    click(window, *undo_button);
    if (!require(controller.undo_count == 1, "undo action reaches the controller")
        || !require(
            workspace->property("renderedGroupCount").toInt() == 2,
            "undo restores separate person groups"
        )) {
        return EXIT_FAILURE;
    }

    const QString screenshot_path = qEnvironmentVariable("SHADOW_PEOPLE_UI_SCREENSHOT");
    if (!screenshot_path.isEmpty()
        && !require(window.grabWindow().save(screenshot_path), "visual snapshot is written")) {
        return EXIT_FAILURE;
    }

    auto* const clear_button =
        workspace->findChild<QQuickItem*>(QStringLiteral("peopleClearButton"));
    if (!require(clear_button != nullptr, "people-data clear action is packaged")) {
        return EXIT_FAILURE;
    }
    click(window, *clear_button);
    QObject* const clear_dialog =
        workspace->findChild<QObject*>(QStringLiteral("clearPeopleDataDialog"));
    if (!require(controller.clear_count == 0, "people data is not cleared without confirmation")
        || !require(
            clear_dialog != nullptr && clear_dialog->property("opened").toBool(),
            "clear action opens a destructive confirmation"
        )
        || !require(QMetaObject::invokeMethod(clear_dialog, "accept"), "clear can be confirmed")) {
        return EXIT_FAILURE;
    }
    drainBindings();
    return require(controller.clear_count == 1, "confirmed clear reaches the controller")
                   && require(
                       workspace->property("renderedGroupCount").toInt() == 0,
                       "clearing removes the people cards"
                   )
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}

#include "people_workspace_contract_test.moc"
