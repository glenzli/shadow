#include <QAbstractItemModel>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QMetaObject>
#include <QObject>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QStandardItemModel>
#include <QString>
#include <QVariant>
#include <QVariantList>

#include <cstdlib>
#include <iostream>
#include <memory>

namespace {

enum PhotoRole {
    PhotoCommitId = Qt::UserRole + 1,
    PhotoName,
    PhotoCreatedAt,
    PhotoParents,
    PhotoRefs,
    PhotoNamed,
    PhotoWorking,
    PhotoRoot,
    PhotoSchema,
    PhotoGradeAdded,
    PhotoGradeRemoved,
    PhotoGradeMoved,
    PhotoGradeModified,
    PhotoOpsAdded,
    PhotoOpsRemoved,
    PhotoOpsModified,
    PhotoParameterBlocks,
    PhotoParameterKeys,
    PhotoOther,
};

enum LibraryRole {
    LibraryCommitId = Qt::UserRole + 101,
    LibraryMessage,
    LibraryCreatedAt,
    LibraryParents,
    LibraryRefs,
    LibraryRoot,
    LibraryHead,
    LibraryPhotos,
    LibrarySharedGrades,
    LibraryMasks,
    LibraryStyles,
    LibraryOutputs,
    LibraryTotal,
};

void drainBindings() {
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}

template <typename Predicate> void waitUntil(Predicate predicate) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 2'000) {
        QCoreApplication::processEvents();
    }
}

bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "History Drawer contract failed: " << message << '\n';
    }
    return condition;
}

void configurePhotoModel(QStandardItemModel& model) {
    model.setItemRoleNames({
        {PhotoCommitId, "commitId"},
        {PhotoName, "name"},
        {PhotoCreatedAt, "createdAtMs"},
        {PhotoParents, "parentCommitIds"},
        {PhotoRefs, "refs"},
        {PhotoNamed, "isNamed"},
        {PhotoWorking, "isWorking"},
        {PhotoRoot, "isRoot"},
        {PhotoSchema, "recipeSchemaChanged"},
        {PhotoGradeAdded, "gradeNodesAdded"},
        {PhotoGradeRemoved, "gradeNodesRemoved"},
        {PhotoGradeMoved, "gradeNodesMoved"},
        {PhotoGradeModified, "gradeNodesModified"},
        {PhotoOpsAdded, "renderOpsAdded"},
        {PhotoOpsRemoved, "renderOpsRemoved"},
        {PhotoOpsModified, "renderOpsModified"},
        {PhotoParameterBlocks, "parameterBlocksChanged"},
        {PhotoParameterKeys, "changedParameterKeys"},
        {PhotoOther, "hasOtherChanges"},
    });
    auto* const item = new QStandardItem;
    item->setData(QStringLiteral("recipe-2"), PhotoCommitId);
    item->setData(QStringLiteral("Second look"), PhotoName);
    item->setData(2'000.0, PhotoCreatedAt);
    item->setData(QStringList{QStringLiteral("recipe-1")}, PhotoParents);
    item->setData(QVariantList{}, PhotoRefs);
    item->setData(true, PhotoNamed);
    item->setData(false, PhotoWorking);
    item->setData(false, PhotoRoot);
    item->setData(false, PhotoSchema);
    item->setData(0, PhotoGradeAdded);
    item->setData(0, PhotoGradeRemoved);
    item->setData(0, PhotoGradeMoved);
    item->setData(1, PhotoGradeModified);
    item->setData(0, PhotoOpsAdded);
    item->setData(0, PhotoOpsRemoved);
    item->setData(0, PhotoOpsModified);
    item->setData(1, PhotoParameterBlocks);
    item->setData(QStringList{QStringLiteral("exposure_stops")}, PhotoParameterKeys);
    item->setData(true, PhotoOther);
    model.appendRow(item);
}

void configureLibraryModel(QStandardItemModel& model) {
    model.setItemRoleNames({
        {LibraryCommitId, "commitId"},
        {LibraryMessage, "message"},
        {LibraryCreatedAt, "createdAtMs"},
        {LibraryParents, "parentCommitIds"},
        {LibraryRefs, "refs"},
        {LibraryRoot, "isRoot"},
        {LibraryHead, "isHead"},
        {LibraryPhotos, "photoChanges"},
        {LibrarySharedGrades, "sharedGradeChanges"},
        {LibraryMasks, "maskChanges"},
        {LibraryStyles, "styleChanges"},
        {LibraryOutputs, "outputStateChanges"},
        {LibraryTotal, "totalChanges"},
    });
    auto* const item = new QStandardItem;
    item->setData(QStringLiteral("library-2"), LibraryCommitId);
    item->setData(QStringLiteral("Second look"), LibraryMessage);
    item->setData(2'000.0, LibraryCreatedAt);
    item->setData(QStringList{QStringLiteral("library-1")}, LibraryParents);
    item->setData(QVariantList{}, LibraryRefs);
    item->setData(false, LibraryRoot);
    item->setData(true, LibraryHead);
    item->setData(1, LibraryPhotos);
    item->setData(0, LibrarySharedGrades);
    item->setData(0, LibraryMasks);
    item->setData(0, LibraryStyles);
    item->setData(0, LibraryOutputs);
    item->setData(1, LibraryTotal);
    model.appendRow(item);
}

} // namespace

class FakeHistoryCoordinator final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString photoId READ photoId NOTIFY photoChanged)
    Q_PROPERTY(QAbstractItemModel* photoModel READ photoModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel* libraryModel READ libraryModel CONSTANT)
    Q_PROPERTY(QVariantList libraryRefs READ libraryRefs CONSTANT)
    Q_PROPERTY(bool photoBusy READ alwaysFalse CONSTANT)
    Q_PROPERTY(bool libraryBusy READ alwaysFalse CONSTANT)
    Q_PROPERTY(bool libraryRefsBusy READ alwaysFalse CONSTANT)
    Q_PROPERTY(bool photoHasMore READ alwaysFalse CONSTANT)
    Q_PROPERTY(bool libraryHasMore READ alwaysFalse CONSTANT)
    Q_PROPERTY(bool libraryRefsHaveMore READ alwaysFalse CONSTANT)
    Q_PROPERTY(QString photoErrorText READ emptyText CONSTANT)
    Q_PROPERTY(QString libraryErrorText READ emptyText CONSTANT)
    Q_PROPERTY(QString libraryRefsErrorText READ emptyText CONSTANT)

  public:
    FakeHistoryCoordinator() {
        configurePhotoModel(photo_model_);
        configureLibraryModel(library_model_);
    }

    QString photoId() const {
        return photo_id_;
    }
    QAbstractItemModel* photoModel() {
        return &photo_model_;
    }
    QAbstractItemModel* libraryModel() {
        return &library_model_;
    }
    QVariantList libraryRefs() const {
        return {QVariantMap{
            {QStringLiteral("name"), QStringLiteral("heads/main")},
            {QStringLiteral("kind"), QStringLiteral("branch")},
            {QStringLiteral("commitId"), QStringLiteral("library-2")},
        }};
    }
    bool alwaysFalse() const noexcept {
        return false;
    }
    QString emptyText() const {
        return {};
    }

    Q_INVOKABLE void openForPhoto(const QString& photo_id) {
        photo_id_ = photo_id;
        ++open_count;
        emit photoChanged();
    }
    Q_INVOKABLE void refreshPhoto() {
        ++refresh_count;
    }
    Q_INVOKABLE void refreshLibrary() {
        ++refresh_count;
    }
    Q_INVOKABLE void refreshLibraryRefs() {
        ++refresh_count;
    }
    Q_INVOKABLE void refreshAll() {
        ++refresh_count;
    }
    Q_INVOKABLE void loadMorePhoto() {}
    Q_INVOKABLE void loadMoreLibrary() {}
    Q_INVOKABLE void loadMoreLibraryRefs() {}

    int open_count = 0;
    int refresh_count = 0;

  signals:
    void photoChanged();

  private:
    QString photo_id_;
    QStandardItemModel photo_model_;
    QStandardItemModel library_model_;
};

class FakeHistoryEditor final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active CONSTANT)
    Q_PROPERTY(QString photoId READ photoId CONSTANT)
    Q_PROPERTY(QString title READ title CONSTANT)
    Q_PROPERTY(bool stateBusy READ stateBusy NOTIFY stateBusyChanged)
    Q_PROPERTY(bool versionDraft READ versionDraft NOTIFY versionDraftChanged)
    Q_PROPERTY(QString editBaseCommitId READ editBaseCommitId NOTIFY editBaseCommitIdChanged)

  public:
    bool active() const noexcept {
        return true;
    }
    QString photoId() const {
        return QStringLiteral("photo-a");
    }
    QString title() const {
        return QStringLiteral("Photo A");
    }
    bool stateBusy() const noexcept {
        return false;
    }
    bool versionDraft() const noexcept {
        return !loaded_commit_id.isEmpty();
    }
    QString editBaseCommitId() const {
        return loaded_commit_id;
    }

    Q_INVOKABLE void saveVersion(const QString& name) {
        saved_name = name;
        ++save_count;
    }
    Q_INVOKABLE void loadVersionDraft(const QString& commit_id) {
        loaded_commit_id = commit_id;
        ++load_count;
        emit versionDraftChanged();
        emit editBaseCommitIdChanged();
    }

    QString saved_name;
    QString loaded_commit_id;
    int save_count = 0;
    int load_count = 0;

  signals:
    void stateBusyChanged();
    void versionDraftChanged();
    void editBaseCommitIdChanged();
    void sourceIdentityChanged();
};

int main(int argc, char* argv[]) {
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QGuiApplication application(argc, argv);
    FakeHistoryCoordinator history;
    FakeHistoryEditor editor;
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("historyFixture"), &history);
    engine.rootContext()->setContextProperty(QStringLiteral("editorFixture"), &editor);

    QQmlComponent component(&engine);
    component.setData(
        R"QML(
            import QtQuick
            import QtQuick.Controls
            import Shadow.HistoryDrawerContract

            ApplicationWindow {
                id: host
                width: 1100
                height: 760
                visible: true

                HistoryDrawer {
                    historyController: historyFixture
                    editor: editorFixture
                    hostWindow: host
                }
            }
        )QML",
        QUrl(QStringLiteral("inmemory:/HistoryDrawerHarness.qml"))
    );
    while (component.status() == QQmlComponent::Loading) {
        QCoreApplication::processEvents();
    }
    if (component.status() == QQmlComponent::Error) {
        std::cerr << component.errorString().toStdString();
        return EXIT_FAILURE;
    }
    std::unique_ptr<QObject> window(component.create());
    if (!window) {
        std::cerr << component.errorString().toStdString();
        return EXIT_FAILURE;
    }
    QObject* const drawer = window->findChild<QObject*>(QStringLiteral("historyDrawer"));
    if (!require(drawer != nullptr, "the packaged Drawer is reachable")) {
        return EXIT_FAILURE;
    }
    const bool presented = QMetaObject::invokeMethod(
        drawer,
        "present",
        Q_ARG(QVariant, QStringLiteral("photo-a")),
        Q_ARG(QVariant, QStringLiteral("Photo A"))
    );
    waitUntil([drawer]() { return drawer->property("opened").toBool(); });
    if (!require(presented, "present is invokable")
        || !require(
            history.open_count == 1 && history.photoId() == QStringLiteral("photo-a")
                && drawer->property("opened").toBool(),
            "opening binds the selected photo and shows the Drawer"
        )) {
        return EXIT_FAILURE;
    }

    QObject* const input = drawer->findChild<QObject*>(QStringLiteral("historyVersionNameInput"));
    QObject* const create =
        drawer->findChild<QObject*>(QStringLiteral("historyCreateVersionButton"));
    QObject* const photo_list = drawer->findChild<QObject*>(QStringLiteral("photoHistoryList"));
    if (!require(
            input != nullptr && create != nullptr && photo_list != nullptr
                && photo_list->property("count").toInt() == 1,
            "the photo timeline and version-creation actions are reachable"
        )) {
        return EXIT_FAILURE;
    }
    input->setProperty("text", QStringLiteral("Named checkpoint"));
    drainBindings();
    QMetaObject::invokeMethod(create, "clicked");
    QMetaObject::invokeMethod(
        drawer,
        "loadPhotoVersion",
        Q_ARG(QVariant, QStringLiteral("recipe-2"))
    );
    drainBindings();
    if (!require(
            editor.save_count == 1 && editor.saved_name == QStringLiteral("Named checkpoint"),
            "Create delegates one complete named-version request"
        )
        || !require(
            editor.load_count == 1 && editor.loaded_commit_id == QStringLiteral("recipe-2")
                && drawer->property("loadedCommitId").toString() == QStringLiteral("recipe-2"),
            "the Drawer reflects the editor's confirmed immutable draft identity"
        )) {
        return EXIT_FAILURE;
    }

    drawer->setProperty("scopeIndex", 1);
    drainBindings();
    QObject* const library_list = drawer->findChild<QObject*>(QStringLiteral("libraryHistoryList"));
    if (!require(
            library_list != nullptr && library_list->property("count").toInt() == 1,
            "Library scope presents the Library-wide commit model"
        )) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

#include "history_drawer_contract_test.moc"
