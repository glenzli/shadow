#include "backend/native_path_input.hpp"
#include "export_task_runner.hpp"
#include "shadow-desktop-bridge/src/lib.rs.h"

#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
QString qstring(const rust::String& s) {
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}
QByteArray read(const QString& path) {
    QFile f(path);
    require(f.open(QIODevice::ReadOnly), "fixture read");
    return f.readAll();
}
void contracts() {
    QTemporaryDir fixture;
    require(fixture.isValid(), "temporary real queue fixture");
    const QString source = fixture.filePath("immutable-source.png");
    QImage image(32, 24, QImage::Format_RGB32);
    image.fill(QColor(50, 110, 150));
    require(image.save(source), "write bounded synthetic original");
    const auto original_bytes = read(source);
    const auto open = [&](const QString& name) {
        const QString root = fixture.filePath(name);
        QDir().mkpath(root);
        return shadow::desktop::open_desktop_session(
            native_path_input::path(root + "/catalog.sqlite"),
            native_path_input::path(root + "/cache")
        );
    };
    std::optional daily{open("daily")};
    auto admitted = (*daily)->admit_pipeline_input(native_path_input::path(source));
    auto backend = std::make_shared<ExportBackend>(**daily);
    QVector<BackendDurableExportTarget> targets;
    for (int i = 0; i < 3; ++i)
        targets.push_back(
            {qstring(admitted.photo_id),
             source,
             fixture.filePath(QString("output-%1.png").arg(i)),
             {},
             {}}
        );
    const auto settings = ExportSettingsCodec::toDurableJson(
        ExportSettingsCodec::fromVariantMap({{"format", "png"}})
    );
    auto cancellation = std::make_shared<std::atomic_bool>(false);
    auto pause = std::make_shared<std::atomic_bool>(false);
    const auto paused = ExportTaskRunner::runDurableExport(
        backend,
        targets,
        settings,
        false,
        cancellation,
        [&](int completed, int, int, int, const QString&) {
            if (completed == 1)
                pause->store(true, std::memory_order_release);
        },
        pause
    );
    if (!paused.paused || paused.completed != 1 || paused.failed != 0)
        std::cerr << "pause result: completed=" << paused.completed << ", failed=" << paused.failed
                  << ", paused=" << paused.paused
                  << ", errors=" << paused.errors.join("; ").toStdString() << '\n';
    require(
        paused.paused && !paused.cancelled && paused.completed == 1 && paused.failed == 0,
        "switch finishes one actual publication and pauses"
    );
    const auto progress = backend->durableExportProgress(paused.job_id);
    require(
        progress.completed == 1 && progress.queued == 2 && progress.cancelled == 0
            && progress.active == 0,
        "remaining immutable work is durable and non-terminal"
    );
    auto debug = open("debug");
    ExportBackend separate(*debug);
    require(!separate.claimNextDurableExportItem(), "other profile cannot claim paused queue");
    backend.reset();
    daily.reset();
    daily.emplace(open("daily"));
    backend = std::make_shared<ExportBackend>(**daily);
    pause->store(false);
    const auto resumed = ExportTaskRunner::runDurableExport(
        backend,
        {},
        {},
        true,
        cancellation,
        [](int, int, int, int, const QString&) {},
        pause
    );
    require(
        !resumed.paused && !resumed.cancelled && resumed.completed == 2 && resumed.failed == 0,
        "same profile reopens and completes only remaining items"
    );
    const auto finished = backend->durableExportProgress(paused.job_id);
    require(
        finished.completed == 3 && finished.cancelled == 0 && finished.queued == 0,
        "resumption preserves full job identity"
    );
    require(read(source) == original_bytes, "pause and resume preserve original bytes");
    for (int i = 0; i < 3; ++i)
        require(
            QFileInfo::exists(fixture.filePath(QString("output-%1.png").arg(i))),
            "real outputs published"
        );
    for (int i = 0; i < 3; ++i)
        targets[i].output_path = fixture.filePath(QString("cancelled-%1.png").arg(i));
    const auto cancelled = ExportTaskRunner::runDurableExport(
        backend,
        targets,
        settings,
        false,
        cancellation,
        [&](int completed, int, int, int, const QString&) {
            if (completed == 1) {
                cancellation->store(true);
                pause->store(true);
            }
        },
        pause
    );
    require(
        cancelled.cancelled && !cancelled.paused && cancelled.cancelled_items == 2,
        "explicit Cancel keeps terminal semantics and priority over switch pause"
    );
}
} // namespace
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        contracts();
        std::cout << "real export pause and recovery contracts passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
