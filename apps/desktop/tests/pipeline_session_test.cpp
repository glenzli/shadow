#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>

namespace {
QByteArray contents(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray{};
}
bool write(const QString& path, const QByteArray& data) {
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
}
bool run(const QString& executable, const QString& scenario) {
    QTemporaryDir root;
    const bool unsupported = scenario == "unsupported";
    const QString first = root.filePath(unsupported ? "first.png" : "first.jpg");
    const QString second = root.filePath("second.jpg");
    QImage image(96, 64, QImage::Format_RGB32);
    image.fill(QColor(90, 120, 140));
    if (!image.save(first))
        return false;
    image.fill(QColor(160, 110, 80));
    if (!image.save(second))
        return false;
    const auto first_bytes = contents(first), second_bytes = contents(second);
    const bool paint = scenario == "paint";
    const bool paint_interaction = scenario == "paint-interaction";
    const bool retry = scenario == "retry";
    const bool direct = scenario == "interactive" || paint || paint_interaction || retry;
    const bool single = scenario == "legacy";
    const bool cancel = scenario == "cancel";
    const QString output1 = root.filePath("output1.png");
    const QString output2 = root.filePath("output2.png");
    const QString result = root.filePath("result.json");
    const QString request = root.filePath("request.json");
    QJsonObject object{
        {"schema", single ? "shadow-pipeline-edit-20260814.1" : "shadow-pipeline-edit-20260920.1"},
        {"requestId", scenario},
        {"export", QJsonObject{{"format", "png"}}}
    };
    if (single) {
        object["input"] = first;
        object["output"] = output1;
    } else
        object["photos"] = QJsonArray{
            QJsonObject{{"input", first}, {"output", output1}},
            QJsonObject{{"input", second}, {"output", output2}}
        };
    if (!write(request, QJsonDocument(object).toJson()))
        return false;
    const QString normal_root = root.filePath("normal-library");
    QDir().mkpath(normal_root);
    const QString sentinel = QDir(normal_root).filePath("catalog.sqlite");
    if (!write(sentinel, "normal Library must stay untouched"))
        return false;
    const QByteArray sentinel_bytes = contents(sentinel);
    auto env = QProcessEnvironment::systemEnvironment();
    env.insert("QT_QPA_PLATFORM", "offscreen");
    env.insert("QT_QUICK_BACKEND", "software");
    env.insert("SHADOW_DESKTOP_DATA_ROOT", normal_root);
    env.insert(
        "SHADOW_PIPELINE_SMOKE_ACTION",
        cancel              ? "cancel"
        : paint_interaction ? "paint-interaction"
        : paint             ? "paint"
        : retry             ? "retry"
                            : "complete"
    );
    env.insert("SHADOW_PIPELINE_SMOKE_OUTPUT", root.path());
    const QString collision = root.filePath("first-edited.png");
    if (direct && !write(collision, "existing output stays unchanged"))
        return false;
    QProcess process;
    process.setProcessEnvironment(env);
    process.setProcessChannelMode(QProcess::MergedChannels);
    const QStringList arguments =
        direct ? QStringList{"--isolate", first, second}
               : QStringList{"--pipeline-edit", "--request", request, "--result", result};
    process.start(executable, arguments);
    if (!process.waitForFinished(110000)) {
        process.kill();
        process.waitForFinished();
        qCritical() << "timeout" << scenario << process.readAll();
        return false;
    }
    const auto diagnostics = process.readAll();
    if (process.exitStatus() != QProcess::NormalExit
        || process.exitCode()
               != (unsupported ? 1
                   : cancel    ? 2
                               : 0)
        || diagnostics.contains("TypeError") || diagnostics.contains("ReferenceError")
        || diagnostics.contains("Binding loop")
        || diagnostics.contains("failed to create component")) {
        qCritical() << scenario << process.exitCode() << diagnostics;
        return false;
    }
    if (contents(first) != first_bytes || contents(second) != second_bytes
        || contents(sentinel) != sentinel_bytes
        || QDir(normal_root).entryList(QDir::Files).size() != 1) {
        qCritical() << "isolation/original invariant failed";
        return false;
    }
    if (direct) {
        if (paint_interaction)
            return diagnostics.contains("Paint interaction acceptance completed");
        if (retry
            && !diagnostics.contains("Partial export edit and retry preserved completed bytes")) {
            qCritical() << "Missing partial recovery acceptance" << diagnostics;
            return false;
        }
        if (contents(collision) != "existing output stays unchanged")
            return false;
        if (QImage(root.filePath("first-edited-1.png")).isNull()
            || QImage(root.filePath("second-edited.png")).isNull())
            return false;
        if (paint) {
            const QImage rendered(root.filePath("first-edited-1.png"));
            const QImage original(first);
            if (rendered.size() != original.size()
                || rendered.pixelColor(48, 32).red() <= original.pixelColor(48, 32).red() + 10
                || std::abs(rendered.pixelColor(0, 0).red() - original.pixelColor(0, 0).red()) > 2
                || !diagnostics.contains(
                    "Paint undo/redo/cancel/erase/persistence/isolation checks passed"
                )) {
                qCritical() << "Paint export did not preserve local coverage" << diagnostics;
                return false;
            }
        }
    } else {
        const auto receipt = QJsonDocument::fromJson(contents(result)).object();
        if (receipt["outcome"]
                != (unsupported ? "failed"
                    : cancel    ? "cancelled"
                                : "completed")
            || receipt["requestId"] != scenario)
            return false;
        if (cancel || unsupported) {
            if (QFile::exists(output1) || QFile::exists(output2))
                return false;
        } else {
            const QImage edited1(output1);
            if (edited1.isNull() || edited1.size() != image.size()
                || edited1.pixelColor(48, 32).red() <= QImage(first).pixelColor(48, 32).red())
                return false;
            if (!single) {
                const QImage edited2(output2);
                if (edited2.isNull() || edited2.size() != image.size()
                    || edited2.pixelColor(48, 32).red() >= QImage(second).pixelColor(48, 32).red())
                    return false;
                const auto photos = receipt["photos"].toArray();
                if (photos.size() != 2 || photos[0].toObject()["outcome"] != "completed"
                    || photos[1].toObject()["outcome"] != "completed")
                    return false;
            }
            if (!diagnostics.contains("edit/switch checks passed"))
                return false;
        }
    }
    qInfo() << "Pipeline packaged session passed:" << scenario;
    return true;
}
} // namespace
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc != 3)
        return 1;
    return run(QString::fromLocal8Bit(argv[1]), QString::fromLocal8Bit(argv[2])) ? 0 : 1;
}
