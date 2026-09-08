#include "lut_export_controller.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QMetaObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>

namespace {
void require(bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(EXIT_FAILURE); }
}

void finish(LutExportController& controller) {
    QElapsedTimer timer;
    timer.start();
    while (controller.busy() && timer.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    require(!controller.busy(), "export did not finish");
}

void cancellation_and_atomic_save() {
    std::atomic_bool cancelled{false};
    auto release = std::make_shared<std::promise<void>>();
    auto gate = release->get_future().share();
    LutExportController controller([&](bool selected_only) {
        return BackendLutExportSnapshot{
            .included_nodes = {selected_only ? QStringLiteral("Selected grade") : QStringLiteral("Captured grade")},
            .can_bake = true,
            .bake = [gate](std::uint16_t) {
                gate.wait();
                return BackendLutExportResult{.document = QByteArray("complete captured LUT")};
            },
            .cancel = [&] { cancelled.store(true); },
        };
    });
    QTemporaryDir directory;
    const QUrl path = QUrl::fromLocalFile(directory.filePath(QStringLiteral("grade.cube")));
    controller.prepare(false);
    controller.bake(17);
    require(controller.busy(), "baking did not become asynchronous");
    controller.cancel();
    controller.prepare(true);
    release->set_value();
    finish(controller);
    require(cancelled.load() && !controller.ready() && !controller.save(path)
            && !QFile::exists(path.toLocalFile()),
        "cancelled late result was published");
    require(controller.canBake() && controller.includedNodes() == QStringList{QStringLiteral("Selected grade")},
        "reopening during cancellation did not prepare the new selection");
    controller.bake(17);
    finish(controller);
    require(controller.ready() && controller.save(path), "completed LUT could not be saved");
    QFile file(path.toLocalFile());
    require(file.open(QIODevice::ReadOnly) && file.readAll() == "complete captured LUT",
        "saved LUT was incomplete");
    const QString original_path = directory.filePath(QStringLiteral("original.jpg"));
    QFile original(original_path);
    require(original.open(QIODevice::WriteOnly), "could not create protected source marker");
    original.write("original bytes");
    original.close();
    require(!controller.save(QUrl::fromLocalFile(original_path)), "LUT save accepted a photo destination");
    require(original.open(QIODevice::ReadOnly) && original.readAll() == "original bytes",
        "LUT export changed source bytes");
}

void packaged_dialog_requires_loss_acceptance() {
    LutExportController controller([](bool) {
        return BackendLutExportSnapshot{
            .included_nodes = {QStringLiteral("Portrait")},
            .omissions = {{QStringLiteral("Portrait"), QStringLiteral("regionalTone")}},
            .can_bake = true,
            .bake = [](std::uint16_t) {
                return BackendLutExportResult{.document = QByteArray("cube"), .probe_count = 4096};
            },
        };
    });
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import QtQuick.Controls
        import Shadow.LutExportContract
        ApplicationWindow {
            id: host
            required property var controller
            width: 900; height: 800; visible: true
            LutExportDialog { objectName: "dialog"; controller: host.controller }
        }
    )", QUrl());
    std::unique_ptr<QObject> window(component.createWithInitialProperties({
        {QStringLiteral("controller"), QVariant::fromValue(&controller)},
    }));
    if (!window) { std::cerr << component.errorString().toStdString(); }
    require(window != nullptr, "packaged LUT dialog failed to instantiate");
    auto* dialog = window->findChild<QObject*>(QStringLiteral("dialog"));
    require(dialog != nullptr && QMetaObject::invokeMethod(dialog, "present", Q_ARG(QVariant, false)),
        "LUT dialog could not present captured grading");
    QCoreApplication::processEvents();
    auto* bake = dialog->findChild<QObject*>(QStringLiteral("lutExportBakeButton"));
    auto* consent = dialog->findChild<QObject*>(QStringLiteral("lutExportLossConsent"));
    auto* save = dialog->findChild<QObject*>(QStringLiteral("lutExportSaveButton"));
    require(bake && consent && save, "LUT dialog actions are unreachable");
    require(!bake->property("enabled").toBool() && !save->property("enabled").toBool(),
        "lossy export was admitted before its omissions were accepted");
    consent->setProperty("checked", true);
    QCoreApplication::processEvents();
    require(bake->property("enabled").toBool(), "loss acceptance did not enable baking");
    controller.bake(17);
    finish(controller);
    require(save->property("enabled").toBool(), "completed export did not enable save");
}
} // namespace

int main(int argc, char** argv) {
    QGuiApplication application(argc, argv);
    cancellation_and_atomic_save();
    packaged_dialog_requires_loss_acceptance();
    std::cout << "LUT export controller and packaged dialog contract passed\n";
}
