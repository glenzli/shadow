#include <QCoreApplication>
#include <QGuiApplication>
#include <QMetaObject>
#include <QObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <cstdlib>
#include <iostream>
#include <memory>

class FakeRemoteLibraryController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(int itemCount READ itemCount CONSTANT)
    Q_PROPERTY(QVariantList remoteLibraries READ remoteLibraries CONSTANT)
    Q_PROPERTY(bool remoteLibraryBusy READ remoteLibraryBusy CONSTANT)
    Q_PROPERTY(QString remoteLibraryStatusCode READ remoteLibraryStatusCode CONSTANT)
    Q_PROPERTY(
        bool remoteLibrarySecureStorageAvailable READ remoteLibrarySecureStorageAvailable CONSTANT
    )

  public:
    [[nodiscard]] int itemCount() const noexcept {
        return 27;
    }

    [[nodiscard]] QVariantList remoteLibraries() const {
        return {
            QVariantMap{
                {QStringLiteral("id"), QStringLiteral("studio-id")},
                {QStringLiteral("address"), QStringLiteral("studio.local:45321")},
                {QStringLiteral("tokenStored"), true},
                {QStringLiteral("serverName"), QStringLiteral("Studio Mac")},
                {QStringLiteral("hasCachedServer"), true},
                {QStringLiteral("photoCount"), 42},
                {QStringLiteral("statusCode"), QStringLiteral("offline-ready")},
                {QStringLiteral("diagnosticText"), QString{}},
                {QStringLiteral("busy"), false},
            },
            QVariantMap{
                {QStringLiteral("id"), QStringLiteral("travel-id")},
                {QStringLiteral("address"), QStringLiteral("travel.local:45321")},
                {QStringLiteral("tokenStored"), true},
                {QStringLiteral("serverName"), QStringLiteral("Travel Mac")},
                {QStringLiteral("hasCachedServer"), false},
                {QStringLiteral("photoCount"), 0},
                {QStringLiteral("statusCode"), QStringLiteral("sync-failed")},
                {QStringLiteral("diagnosticText"), QStringLiteral("offline")},
                {QStringLiteral("busy"), false},
            },
        };
    }

    [[nodiscard]] bool remoteLibraryBusy() const noexcept {
        return false;
    }

    [[nodiscard]] QString remoteLibraryStatusCode() const {
        return {};
    }

    [[nodiscard]] bool remoteLibrarySecureStorageAvailable() const noexcept {
        return true;
    }

    Q_INVOKABLE QString saveRemoteLibraryConnection(
        const QString& connection_id,
        const QString& address,
        const QString& token
    ) {
        saved_connection_id = connection_id;
        saved_address = address;
        saved_token = token;
        return QStringLiteral("added-id");
    }

    Q_INVOKABLE bool removeRemoteLibraryConnection(const QString& connection_id) {
        removed_connection_id = connection_id;
        return true;
    }

    Q_INVOKABLE void syncRemoteLibrary(const QString& connection_id) {
        synced_connection_id = connection_id;
    }

    Q_INVOKABLE void syncAllRemoteLibraries() {
        ++sync_all_count;
    }

    QString saved_connection_id;
    QString saved_address;
    QString saved_token;
    QString removed_connection_id;
    QString synced_connection_id;
    int sync_all_count = 0;
};

namespace {

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "remote Library management pane contract failed: " << message << '\n';
    }
    return condition;
}

void drainBindings() {
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}

void collectVisualChildren(
    QQuickItem* const parent,
    const QString& object_name,
    QList<QQuickItem*>& result
) {
    for (QQuickItem* const child : parent->childItems()) {
        if (child->objectName() == object_name) {
            result.push_back(child);
        }
        collectVisualChildren(child, object_name, result);
    }
}

} // namespace

int main(int argc, char* argv[]) {
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QGuiApplication application(argc, argv);
    QQmlEngine engine;
    QQmlComponent component{&engine};
    component.loadFromModule(
        QStringLiteral("Shadow.LibraryRemoteConnectionsContract"),
        QStringLiteral("LibraryRemoteConnectionsPane")
    );

    FakeRemoteLibraryController controller;
    std::unique_ptr<QObject> pane{component.createWithInitialProperties({
        {QStringLiteral("controller"), QVariant::fromValue(static_cast<QObject*>(&controller))},
        {QStringLiteral("width"), 760.0},
    })};
    if (!pane) {
        std::cerr << component.errorString().toStdString();
        return EXIT_FAILURE;
    }
    QQuickWindow window;
    window.resize(760, 900);
    auto* const pane_item = qobject_cast<QQuickItem*>(pane.get());
    if (!require(pane_item != nullptr, "the packaged pane creates a visual item")) {
        return EXIT_FAILURE;
    }
    pane_item->setParentItem(window.contentItem());
    window.show();
    drainBindings();

    QList<QQuickItem*> sync_buttons;
    collectVisualChildren(pane_item, QStringLiteral("remoteLibrarySyncButton"), sync_buttons);
    QObject* const sync_all =
        pane->findChild<QObject*>(QStringLiteral("remoteLibrarySyncAllButton"));
    QList<QQuickItem*> remove_buttons;
    collectVisualChildren(pane_item, QStringLiteral("remoteLibraryRemoveButton"), remove_buttons);
    QObject* const remove = remove_buttons.isEmpty() ? nullptr : remove_buttons.front();
    QList<QQuickItem*> edit_buttons;
    collectVisualChildren(pane_item, QStringLiteral("remoteLibraryEditButton"), edit_buttons);
    QObject* const edit = edit_buttons.isEmpty() ? nullptr : edit_buttons.front();
    QObject* const edit_address =
        pane->findChild<QObject*>(QStringLiteral("remoteLibraryEditAddressField"));
    QObject* const edit_token =
        pane->findChild<QObject*>(QStringLiteral("remoteLibraryEditTokenField"));
    QObject* const edit_save =
        pane->findChild<QObject*>(QStringLiteral("remoteLibraryEditSaveButton"));
    QObject* const remove_confirm =
        pane->findChild<QObject*>(QStringLiteral("remoteLibraryRemoveConfirmButton"));
    QObject* const address =
        pane->findChild<QObject*>(QStringLiteral("remoteLibraryServerAddressField"));
    QObject* const token =
        pane->findChild<QObject*>(QStringLiteral("remoteLibraryAccessTokenField"));
    QObject* const add =
        pane->findChild<QObject*>(QStringLiteral("remoteLibraryConnectionAddButton"));
    if (!require(sync_buttons.size() == 2, "every configured Library has its own sync action")
        || !require(
            sync_all != nullptr && sync_all->property("visible").toBool(),
            "several Libraries expose one sync-all action"
        )
        || !require(
            remove != nullptr && remove_confirm != nullptr && edit != nullptr
                && edit_address != nullptr && edit_token != nullptr && edit_save != nullptr
                && address != nullptr && token != nullptr && add != nullptr,
            "the packaged pane exposes editing, removal, and secure connection admission"
        )) {
        return EXIT_FAILURE;
    }

    if (!require(
            QMetaObject::invokeMethod(sync_buttons.front(), "clicked"),
            "a Library sync action accepts a click"
        )) {
        return EXIT_FAILURE;
    }
    drainBindings();
    if (!require(
            controller.synced_connection_id == QStringLiteral("studio-id"),
            "sync routes through the selected stable connection identity"
        )) {
        return EXIT_FAILURE;
    }

    if (!require(QMetaObject::invokeMethod(edit, "clicked"), "edit action opens the connection")
        || !require(
            edit_address->property("text").toString() == QStringLiteral("studio.local:45321"),
            "editing preserves the selected server address"
        )) {
        return EXIT_FAILURE;
    }
    edit_address->setProperty("text", QStringLiteral("studio-new.local:45321"));
    drainBindings();
    if (!require(
            edit_save->property("enabled").toBool(),
            "an existing connection may retain its saved token"
        )
        || !require(
            QMetaObject::invokeMethod(edit_save, "clicked"),
            "edited connection accepts save and sync"
        )) {
        return EXIT_FAILURE;
    }
    drainBindings();
    if (!require(
            controller.saved_connection_id == QStringLiteral("studio-id")
                && controller.saved_address == QStringLiteral("studio-new.local:45321")
                && controller.saved_token.isEmpty(),
            "editing routes through the selected stable identity without exposing its token"
        )) {
        return EXIT_FAILURE;
    }

    address->setProperty("text", QStringLiteral("new.local:45321"));
    token->setProperty("text", QStringLiteral("01234567890123456789012345678901"));
    drainBindings();
    if (!require(add->property("enabled").toBool(), "valid address and token admit a connection")
        || !require(QMetaObject::invokeMethod(add, "clicked"), "add action accepts a click")) {
        return EXIT_FAILURE;
    }
    drainBindings();
    if (!require(
            controller.saved_connection_id.isEmpty()
                && controller.saved_address == QStringLiteral("new.local:45321")
                && controller.saved_token.size() == 32,
            "adding a Library uses a new stable identity and the entered credential"
        )) {
        return EXIT_FAILURE;
    }

    if (!require(QMetaObject::invokeMethod(remove, "clicked"), "remove action opens confirmation")
        || !require(
            QMetaObject::invokeMethod(remove_confirm, "clicked"),
            "confirmed removal accepts a click"
        )) {
        return EXIT_FAILURE;
    }
    drainBindings();
    return require(
               controller.removed_connection_id == QStringLiteral("studio-id"),
               "removal routes through only the selected stable connection identity"
           )
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}

#include "library_remote_connections_pane_test.moc"
