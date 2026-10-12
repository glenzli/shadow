#include "storage_profile_controller.hpp"

#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QJSValue>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QTranslator>

#include <cstdlib>
#include <iostream>
#include <stdexcept>

class CloseEditor final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString autosaveErrorText READ autosaveErrorText CONSTANT)
    Q_PROPERTY(bool stateBusy READ stateBusy CONSTANT)
  public:
    QString autosaveErrorText() const {
        return "fixture disk full";
    }
    bool stateBusy() const {
        return false;
    }
    Q_INVOKABLE bool prepareToClose() const {
        return true;
    }
  signals:
    void closeReady();
    void closeSaveFailed();
    void photoSwitchSaveFailed();
};

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
QQuickItem* item(QObject* root, const char* name) {
    auto* found = root->findChild<QQuickItem*>(QString::fromLatin1(name));
    require(found != nullptr, name);
    return found;
}
void click(QQuickWindow* window, QQuickItem* target) {
    require(target->isVisible() && target->isEnabled(), "real pointer target is actionable");
    QTest::mouseClick(
        window,
        Qt::LeftButton,
        Qt::NoModifier,
        target->mapToScene(QPointF(target->width() / 2, target->height() / 2)).toPoint()
    );
    QTest::qWait(30);
}
QQuickItem* recentRow(QQuickItem* parent, const QString& root) {
    if (parent->objectName() == "recentLibraryRow") {
        const auto value = parent->property("modelData");
        const auto map = value.metaType() == QMetaType::fromType<QJSValue>()
                             ? value.value<QJSValue>().toVariant().toMap()
                             : value.toMap();
        if (map.value("root").toString() == root)
            return parent;
    }
    for (auto* child : parent->childItems())
        if (auto* row = recentRow(child, root))
            return row;
    return nullptr;
}
void contracts() {
    QTemporaryDir fixture;
    const QString base = QFileInfo(fixture.path()).canonicalFilePath();
    StorageProfileRegistry registry(base + "/daily", base + "/registry");
    StorageProfileLaunch launch;
    launch.daily_root = registry.dailyRoot();
    launch.debug_build = true;
    launch.arguments = {"Shadow"};
    QString error;
    auto current = StorageProfile::open(launch, &error);
    require(current != nullptr, "current profile fixture");
    require(registry.remember(current->info(), "debug", &error), "current recent fixture");
    auto target = registry.create("Pointer Test Library", base, &error);
    require(target && registry.remember(*target, {}, &error), "target recent fixture");
    bool quiet = false;
    int starts = 0;
    int archives = 0;
    StorageProfileArchiveRequest archive_request;
    StorageProfileController* switch_owner = nullptr;
    StorageProfileController manager(
        *current,
        registry,
        "debug",
        {
            .blocker = {},
            .suspend_background = [](bool) {},
            .jobs_quiet = [&] { return quiet; },
            .prepare_editor = [] { return false; },
            .restart =
                [&](const auto&, const auto&) {
                    ++starts;
                    switch_owner->restartFailed("fixture immediate launch failure");
                },
            .archive_blocker = {},
            .archive =
                [&](const auto& request) {
                    ++archives;
                    archive_request = request;
                },
            .cancel_archive = [&] { manager.archiveFailed("fixture cancelled"); },
        }
    );
    switch_owner = &manager;
    CloseEditor close_editor;
    QObject::connect(
        &close_editor,
        &CloseEditor::closeReady,
        &manager,
        &StorageProfileController::editorReady
    );
    QObject::connect(&close_editor, &CloseEditor::closeSaveFailed, &manager, [&] {
        manager.editorSaveFailed(close_editor.autosaveErrorText());
    });
    QQmlEngine engine;
    bool warnings = false;
    QObject::connect(&engine, &QQmlEngine::warnings, [&](const QList<QQmlError>& errors) {
        warnings = true;
        for (const auto& e : errors)
            std::cerr << e.toString().toStdString() << '\n';
    });
    engine.rootContext()->setContextProperty("manager", &manager);
    engine.rootContext()->setContextProperty("closeEditor", &close_editor);
    QQmlComponent component(&engine);
    component.setData(
        R"QML(
import QtQuick
import QtQuick.Controls
import Shadow.StorageProfilesContract
ApplicationWindow {
    id: host
    width: 900; height: 780; visible: true
    Component.onCompleted: Theme.effectiveDark = true
    StorageProfilesDialog {
        objectName: "profiles"
        profileManager: manager
        hostWidth: parent.width
        hostHeight: parent.height
    }
    AutosaveFailureRecovery {
        objectName: "closeRecovery"
        parent: Overlay.overlay
        editor: closeEditor
        hostWindow: host
        profileSwitchPending: manager.busy
    }
}
)QML",
        {}
    );
    std::unique_ptr<QObject> root(component.create());
    if (!root)
        throw std::runtime_error(component.errorString().toStdString());
    auto* window = qobject_cast<QQuickWindow*>(root.get());
    auto* dialog = root->findChild<QObject*>("profiles");
    require(window && dialog && QMetaObject::invokeMethod(dialog, "open"), "packaged dialog opens");
    window->requestActivate();
    QTest::qWait(100);
    window->grabWindow().save("/private/tmp/shadow-profile-protection-dialog-initial.png");
    auto* name = item(root.get(), "currentLibraryName");
    click(window, name);
    // The offscreen platform uses Qt's portable Control+A key binding.
    QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
    QGuiApplication::inputMethod()->commit();
    QInputMethodEvent rename_input;
    rename_input.setCommitString("Development Session");
    QCoreApplication::sendEvent(window, &rename_input);
    QTest::qWait(10);
    click(window, item(root.get(), "renameLibraryButton"));
    if (manager.currentName() != "Development Session")
        throw std::runtime_error(
            "pointer rename: field=" + name->property("text").toString().toStdString() + ", name="
            + manager.currentName().toStdString() + ", error=" + manager.errorText().toStdString()
        );
    auto* target_row = recentRow(item(root.get(), "recentLibraries"), target->root);
    require(target_row != nullptr, "recent target delegate exists");
    click(window, target_row);
    click(window, item(root.get(), "openSelectedLibraryButton"));
    require(
        manager.busy() && manager.canCancel() && starts == 0,
        "real selection respects background wait"
    );
    require(
        !item(root.get(), "closeLibrariesButton")->isEnabled(),
        "modal close disabled while switch owned"
    );
    click(window, item(root.get(), "cancelLibrarySwitchButton"));
    require(!manager.busy() && window->isVisible(), "pointer cancellation keeps current window");
    quiet = true;
    click(window, item(root.get(), "openSelectedLibraryButton"));
    require(manager.busy() && starts == 0, "real selection waits for save acknowledgement");
    emit close_editor.closeSaveFailed();
    QTest::qWait(20);
    require(
        !manager.busy() && starts == 0 && window->isVisible(),
        "save failure preserves actual window"
    );
    require(
        item(root.get(), "libraryErrorText")
            ->property("text")
            .toString()
            .contains("fixture disk full"),
        "failure remains visible in management dialog"
    );
    const QImage image = window->grabWindow();
    require(!image.isNull(), "real dialog renders");
    image.save("/private/tmp/shadow-profile-protection-dialog-contract.png");
    auto* recovery = root->findChild<QObject*>("closeRecovery");
    require(
        recovery && !recovery->property("visible").toBool(),
        "profile save failure does not expose the quit-without-saving path"
    );
    click(window, item(root.get(), "openSelectedLibraryButton"));
    emit close_editor.closeReady();
    QTest::qWait(30);
    require(
        !manager.busy() && starts == 1 && window->isVisible(),
        "immediate target launch failure cannot trigger normal autosave-close listeners"
    );
    require(
        !recovery->property("visible").toBool(),
        "target failure keeps normal quit recovery inactive"
    );
    quiet = false;
    click(window, item(root.get(), "newLibraryButton"));
    auto* new_name = item(root.get(), "newLibraryName");
    click(window, new_name);
    QInputMethodEvent new_name_input;
    new_name_input.setCommitString("Created Through UI");
    QCoreApplication::sendEvent(window, &new_name_input);
    dialog->setProperty("newParent", QUrl::fromLocalFile(base));
    click(window, item(root.get(), "openSelectedLibraryButton"));
    require(
        manager.busy() && manager.canCancel(),
        "pointer creation starts the same safe switch gate"
    );
    QString registry_error;
    const auto created = registry.recent(&registry_error).front();
    require(
        created.name() == "Created Through UI" && created.root != current->root()
            && !QFileInfo::exists(created.root + "/catalog.sqlite"),
        "pointer creation makes an independent empty named library"
    );
    click(window, item(root.get(), "cancelLibrarySwitchButton"));
    require(
        !manager.busy() && manager.currentRoot() == current->root(),
        "cancelling creation switch retains current session"
    );
    require(!warnings, "packaged dialog has no QML warnings");
    click(window, item(root.get(), "newLibraryButton"));
    click(window, item(root.get(), "testCopyLibraryButton"));
    require(
        dialog->property("creationMode").toInt() == 1,
        "pointer chooses independent test copy form"
    );
    dialog->setProperty("newParent", QUrl::fromLocalFile(base));
    quiet = true;
    click(window, item(root.get(), "openSelectedLibraryButton"));
    require(manager.busy() && archives == 0, "pointer test copy waits for durable save");
    emit close_editor.closeReady();
    QTest::qWait(30);
    require(
        manager.busy() && archives == 1 && archive_request.action == 2 && window->isVisible(),
        "actual closeReady starts test copy without closing the window"
    );
    click(window, item(root.get(), "cancelLibrarySwitchButton"));
    require(
        !manager.busy() && window->isVisible(),
        "pointer cancellation preserves actual window after worker cleanup"
    );
    require(!warnings, "new data-protection form has no QML warnings");
    QTranslator translator;
    require(
        translator.load(QString::fromUtf8(SHADOW_PROFILE_TRANSLATION)),
        "load production Chinese catalog"
    );
    QCoreApplication::installTranslator(&translator);
    engine.retranslate();
    QTest::qWait(30);
    require(
        item(root.get(), "backupLibraryButton")->property("text").toString()
                == QString::fromUtf8("备份…")
            && item(root.get(), "testCopyLibraryButton")->property("text").toString()
                   == QString::fromUtf8("测试副本…")
            && dialog->property("title").toString() == QString::fromUtf8("资料库"),
        "new actions retranslate to Chinese on the same live tree"
    );
    window->grabWindow().save("/private/tmp/shadow-profile-protection-dialog-chinese.png");
    QCoreApplication::removeTranslator(&translator);
    engine.retranslate();
    QTest::qWait(30);
    require(
        item(root.get(), "backupLibraryButton")->property("text").toString() == "Back Up…",
        "same live actions return to English"
    );
    emit close_editor.closeReady();
    QTest::qWait(30);
    require(!window->isVisible(), "ordinary saved window close still works outside switching");
}

} // namespace
int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle("Basic");
    try {
        contracts();
        std::cout << "profile QML pointer contracts passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

#include "storage_profiles_qml_test.moc"
