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
        return false;
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
                {QStringLiteral("displayIndex"), 1},
                {QStringLiteral("photoCount"), 3},
                {QStringLiteral("thumbnailSource"), QString{}},
                {QStringLiteral("selected"), selected_.contains(QStringLiteral("a"))},
                {QStringLiteral("merged"), false},
            },
            QVariantMap{
                {QStringLiteral("groupId"), QStringLiteral("b")},
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
    Q_INVOKABLE void clearSessionResults() {
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
    Q_INVOKABLE void undoLastMerge() {
        if (!can_undo_merge_) {
            return;
        }
        ++undo_count;
        merged_ = false;
        can_undo_merge_ = false;
        emit changed();
    }

    int start_count = 0;
    int clear_count = 0;
    int merge_count = 0;
    int undo_count = 0;

  signals:
    void changed();

  private:
    bool has_results_ = false;
    bool merged_ = false;
    bool can_undo_merge_ = false;
    QSet<QString> selected_;
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
    std::unique_ptr<QObject> object{component.createWithInitialProperties({
        {QStringLiteral("controller"), QVariant::fromValue(&controller)},
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
    click(window, *start_button);
    if (!require(controller.start_count == 1, "manual action reaches the controller")
        || !require(
            workspace->property("renderedGroupCount").toInt() == 2,
            "anonymous group cards follow session results"
        )) {
        return EXIT_FAILURE;
    }

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
    if (!require(clear_button != nullptr, "session clear action is packaged")) {
        return EXIT_FAILURE;
    }
    click(window, *clear_button);
    return require(controller.clear_count == 1, "clear action reaches the controller")
                   && require(
                       workspace->property("renderedGroupCount").toInt() == 0,
                       "clearing removes the session cards"
                   )
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}

#include "people_workspace_contract_test.moc"
