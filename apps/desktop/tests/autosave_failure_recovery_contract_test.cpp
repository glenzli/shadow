#include <QCoreApplication>
#include <QGuiApplication>
#include <QMetaObject>
#include <QObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QString>
#include <QUrl>
#include <QVariant>

#include <cstdlib>
#include <iostream>
#include <memory>

class FakeAutosaveEditor final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString autosaveErrorText READ autosaveErrorText CONSTANT)
    Q_PROPERTY(bool stateBusy READ stateBusy CONSTANT)

public:
    using QObject::QObject;

    [[nodiscard]] QString autosaveErrorText() const {
        return QStringLiteral("durable write rejected");
    }
    [[nodiscard]] bool stateBusy() const noexcept { return false; }

    Q_INVOKABLE bool prepareToClose() {
        ++prepare_count;
        return prepare_result;
    }
    Q_INVOKABLE void cancelPendingPhotoOpen() {
        ++cancel_pending_count;
    }
    Q_INVOKABLE void retryAutosave() {
        ++retry_count;
    }
    Q_INVOKABLE bool discardFailedAutosaveAndOpenPendingPhoto() {
        ++discard_and_open_count;
        return true;
    }

    bool prepare_result = false;
    int prepare_count = 0;
    int cancel_pending_count = 0;
    int retry_count = 0;
    int discard_and_open_count = 0;

signals:
    void closeReady();
    void closeSaveFailed();
    void photoSwitchSaveFailed();
};

class FakeHostWindow final : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

    Q_INVOKABLE void close() {
        ++close_count;
    }

    int close_count = 0;
};

namespace {

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "autosave recovery contract failed: "
                  << message << '\n';
    }
    return condition;
}

[[nodiscard]] bool invoke(QObject* target, const char* method) {
    return QMetaObject::invokeMethod(target, method);
}

[[nodiscard]] bool invokeBoolean(
    QObject* target,
    const char* method,
    bool* result
) {
    QVariant returned;
    const bool invoked = QMetaObject::invokeMethod(
        target,
        method,
        Q_RETURN_ARG(QVariant, returned)
    );
    *result = returned.toBool();
    return invoked;
}

void drainDeferredActions() {
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}

} // namespace

int main(int argc, char* argv[]) {
    QGuiApplication application(argc, argv);
    QQmlEngine engine;
    QQmlComponent component(
        &engine,
        QUrl::fromLocalFile(QStringLiteral(
            SHADOW_DESKTOP_SOURCE_DIR "/qml/AutosaveFailureRecovery.qml"
        ))
    );
    FakeAutosaveEditor editor;
    FakeHostWindow host;
    std::unique_ptr<QObject> recovery(component.createWithInitialProperties({
        {QStringLiteral("editor"), QVariant::fromValue(&editor)},
        {QStringLiteral("hostWindow"), QVariant::fromValue(&host)},
    }));
    if (!recovery) {
        std::cerr << component.errorString().toStdString();
        return EXIT_FAILURE;
    }

    bool close_accepted = true;
    if (!require(
            invokeBoolean(
                recovery.get(),
                "shouldAcceptClose",
                &close_accepted
            ),
            "close admission is invokable"
        )
        || !require(
            !close_accepted
                && editor.prepare_count == 1,
            "a failed durable close attempt remains intercepted"
        )) {
        return EXIT_FAILURE;
    }

    recovery->setProperty("closeAfterAutosave", true);
    editor.prepare_count = 0;
    close_accepted = false;
    if (!require(
            invokeBoolean(
                recovery.get(),
                "shouldAcceptClose",
                &close_accepted
            )
                && close_accepted
                && editor.prepare_count == 0,
            "an explicitly resolved close bypasses another save attempt"
        )) {
        return EXIT_FAILURE;
    }
    recovery->setProperty("closeAfterAutosave", false);

    emit editor.closeSaveFailed();
    drainDeferredActions();
    if (!require(
            !recovery->property("openingPendingPhoto").toBool(),
            "close failure selects the quit-recovery presentation"
        )
        || !require(
            invoke(recovery.get(), "retrySave"),
            "retry action is invokable"
        )) {
        return EXIT_FAILURE;
    }
    drainDeferredActions();
    if (!require(
            editor.retry_count == 1 && host.close_count == 1,
            "close retry restarts autosave and re-enters the close path"
        )) {
        return EXIT_FAILURE;
    }

    emit editor.photoSwitchSaveFailed();
    drainDeferredActions();
    if (!require(
            recovery->property("openingPendingPhoto").toBool()
                && invoke(recovery.get(), "keepEditing")
                && editor.cancel_pending_count == 1,
            "keep editing cancels only the queued photo switch"
        )) {
        return EXIT_FAILURE;
    }

    emit editor.photoSwitchSaveFailed();
    drainDeferredActions();
    if (!require(
            invoke(recovery.get(), "continueWithoutSaving")
                && editor.discard_and_open_count == 1,
            "photo-switch bypass discards the in-memory draft and opens the queue"
        )) {
        return EXIT_FAILURE;
    }

    emit editor.closeSaveFailed();
    drainDeferredActions();
    if (!require(
            invoke(recovery.get(), "continueWithoutSaving"),
            "quit bypass is invokable"
        )) {
        return EXIT_FAILURE;
    }
    drainDeferredActions();
    if (!require(
            recovery->property("closeAfterAutosave").toBool()
                && host.close_count == 2,
            "quit bypass is reachable only after a surfaced save failure"
        )) {
        return EXIT_FAILURE;
    }

    recovery->setProperty("closeAfterAutosave", false);
    emit editor.closeReady();
    drainDeferredActions();
    if (!require(
            recovery->property("closeAfterAutosave").toBool()
                && host.close_count == 3,
            "a durable completion releases the intercepted close exactly once"
        )) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

#include "autosave_failure_recovery_contract_test.moc"
