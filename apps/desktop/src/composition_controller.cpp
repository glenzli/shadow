#include "composition_controller.hpp"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QtConcurrent>
#include <cerrno>
#include <filesystem>
#ifdef Q_OS_MACOS
#include <stdio.h>
#endif
#ifdef Q_OS_WIN
#include <windows.h>
#endif
namespace {
std::filesystem::path nativePath(const QString& path) {
#ifdef _WIN32
    return std::filesystem::path(path.toStdWString());
#else
    auto p = path.toUtf8();
    return std::filesystem::path(std::string(p.constData(), size_t(p.size())));
#endif
}
} // namespace
CompositionController::CompositionController(QObject* p) :
    QObject(p), cancelled_(std::make_shared<std::atomic_bool>(false)) {
    connect(&process_, &QProcess::readyReadStandardOutput, this, &CompositionController::receive);
    connect(&process_, &QProcess::readyReadStandardError, this, [this] {
        process_.readAllStandardError();
    });
    connect(
        &process_,
        qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
        this,
        &CompositionController::finished
    );
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            running_ = false;
            error_ = errorMessage("worker-unavailable");
            emit changed();
        }
    });
    connect(&publication_, &QFutureWatcher<Publication>::finished, this, [this] {
        const auto result = publication_.result();
        running_ = false;
        if (!result.path.isEmpty()) {
            saved_path_ = result.path;
            ready_ = false;
            phase_ = "saved";
            emit saved(QUrl::fromLocalFile(result.path));
        } else if (!cancelled_->load())
            error_ = errorMessage(result.error);
        else {
            ready_ = false;
            phase_ = "cancelled";
            staging_.reset();
        }
        emit changed();
    });
}
CompositionController::~CompositionController() {
    cancelled_->store(true);
    process_.kill();
    process_.waitForFinished(3000);
    publication_.waitForFinished();
}
bool CompositionController::busy() const {
    return running_;
}
bool CompositionController::ready() const {
    return ready_ && !running_;
}
QString CompositionController::error() const {
    return error_;
}
QString CompositionController::savedPath() const {
    return saved_path_;
}
int CompositionController::inputCount() const {
    return int(inputs_.size());
}
QUrl CompositionController::preview() const {
    return staging_ && width_ > 0 ? QUrl::fromLocalFile(staging_->filePath("preview.jpg")) : QUrl{};
}
QString CompositionController::dimensions() const {
    return width_ ? tr("%1 × %2 pixels · 32-bit linear TIFF").arg(width_).arg(height_) : QString{};
}
QString CompositionController::status() const {
    if (phase_ == "decode")
        return tr("Preparing photo %1 of %2…").arg(index_ + 1).arg(inputs_.size());
    if (phase_ == "align")
        return tr("Aligning overlapping photos…");
    if (phase_ == "blend")
        return tr("Blending photo %1 of %2…").arg(index_ + 1).arg(inputs_.size());
    if (phase_ == "write")
        return tr("Preparing the composite preview…");
    if (phase_ == "ready")
        return tr("Review the result before saving.");
    if (phase_ == "saving")
        return tr("Saving the composite…");
    if (phase_ == "saved")
        return tr("Saved. Importing the composite into the Library.");
    if (phase_ == "cancelled")
        return tr("Composition cancelled.");
    return tr("%1 source photos selected.").arg(inputs_.size());
}
QString CompositionController::errorMessage(const QString& code) const {
    if (code == "input-count" || code == "request-invalid")
        return tr("Select between 2 and 12 photos.");
    if (code == "input-unavailable")
        return tr("An original is unavailable. Download or reconnect it, then retry.");
    if (code == "duplicate-input")
        return tr("Each source photo must be selected only once.");
    if (code == "hdr-metadata")
        return tr("HDR requires RAW photos with shutter speed, aperture and ISO metadata.");
    if (code == "hdr-camera")
        return tr("Use an exposure bracket from the same camera with fixed aperture and ISO.");
    if (code == "hdr-white-balance")
        return tr(
            "The bracket has different white balances. Use photos captured with a fixed white "
            "balance."
        );
    if (code == "hdr-exposures")
        return tr("The photos do not contain a sufficient exposure bracket.");
    if (code == "hdr-dimensions")
        return tr("HDR source photos must have the same dimensions and orientation.");
    if (code == "hdr-alignment")
        return tr(
            "The bracket could not be aligned reliably. Try tripod photos or disable alignment."
        );
    if (code == "panorama-overlap" || code == "panorama-geometry" || code == "alignment-failed")
        return tr(
            "All photos could not be joined reliably. Use overlapping views from the same scene."
        );
    if (code == "memory-budget")
        return tr(
            "This set exceeds the composition memory limit. Choose 2048 pixels or fewer photos."
        );
    if (code == "input-changed")
        return tr(
            "An original changed during composition. The result was discarded; please retry."
        );
    if (code == "output-conflict")
        return tr("A file already exists at that destination. Choose a new name.");
    if (code == "worker-unavailable")
        return tr(
            "The photo composition worker is unavailable. Reinstall the complete application."
        );
    if (code == "output-unavailable")
        return tr("The composite could not be saved. Check the destination and available space.");
    return tr("Composition failed. Check the source files and try again.");
}
void CompositionController::prepare(const QVariantList& targets, const QString& mode) {
    if (busy())
        return;
    staging_.reset();
    ready_ = false;
    width_ = height_ = 0;
    saved_path_.clear();
    phase_.clear();
    error_.clear();
    inputs_ = targets;
    mode_ = mode;
    emit changed();
}
void CompositionController::start(int max_edge, bool align, bool deghost, bool compensate) {
    if (busy())
        return;
    ready_ = false;
    width_ = height_ = 0;
    error_.clear();
    saved_path_.clear();
    pending_.clear();
    if (inputs_.size() < 2 || inputs_.size() > 12) {
        error_ = errorMessage("input-count");
        emit changed();
        return;
    }
    staging_ = std::make_unique<QTemporaryDir>(QDir::tempPath() + "/shadow-composition-XXXXXX");
    if (!staging_->isValid()) {
        error_ = errorMessage("output-unavailable");
        emit changed();
        return;
    }
    QJsonArray sources;
    for (auto v : inputs_) {
        auto t = v.toMap();
        sources.append(
            QJsonObject{
                {"path", t.value("sourcePath").toString()},
                {"photoId", t.value("photoId").toString()},
                {"representationId", t.value("representationId").toString()}
            }
        );
    }
    QJsonObject request{
        {"mode", mode_},
        {"inputs", sources},
        {"maxEdge", max_edge},
        {"align", align},
        {"deghost", deghost},
        {"compensate", compensate}
    };
    QFile file(staging_->filePath("request.json"));
    auto bytes = QJsonDocument(request).toJson(QJsonDocument::Compact);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
        error_ = errorMessage("output-unavailable");
        emit changed();
        return;
    }
    file.close();
    cancelled_->store(false);
    running_ = true;
    phase_ = "decode";
    index_ = 0;
    QString worker =
        QDir(QCoreApplication::applicationDirPath()).filePath("shadow-photo-composition-worker");
#ifdef Q_OS_WIN
    worker += ".exe";
#endif
    process_.start(worker, {file.fileName(), staging_->path()});
    emit changed();
}
void CompositionController::receive() {
    pending_ += process_.readAllStandardOutput();
    if (pending_.size() > 64 * 1024) {
        error_ = errorMessage("invalid-input");
        process_.kill();
        return;
    }
    while (pending_.contains('\n')) {
        const qsizetype end = pending_.indexOf('\n');
        auto line = pending_.left(end);
        pending_.remove(0, end + 1);
        auto event = QJsonDocument::fromJson(line).object();
        if (cancelled_->load())
            continue;
        QString phase = event.value("phase").toString();
        if (phase == "error")
            error_ = errorMessage(event.value("code").toString());
        else if (phase == "ready") {
            width_ = event.value("width").toInt();
            height_ = event.value("height").toInt();
        }
        phase_ = phase;
        index_ = event.value("index").toInt();
        emit changed();
    }
}
void CompositionController::finished(int code, QProcess::ExitStatus status) {
    receive();
    running_ = false;
    if (cancelled_->load()) {
        phase_ = "cancelled";
        ready_ = false;
        staging_.reset();
        width_ = height_ = 0;
    } else if (
        code == 0 && status == QProcess::NormalExit && width_ > 0 && height_ > 0 && staging_
        && QFileInfo(staging_->filePath("result.tif")).isFile()
    ) {
        ready_ = true;
        phase_ = "ready";
    } else {
        ready_ = false;
        width_ = height_ = 0;
        if (error_.isEmpty())
            error_ = errorMessage("invalid-input");
    }
    emit changed();
}
void CompositionController::cancel() {
    cancelled_->store(true);
    if (process_.state() != QProcess::NotRunning)
        process_.kill();
    if (!busy()) {
        ready_ = false;
        staging_.reset();
        width_ = height_ = 0;
        emit changed();
    }
}
QUrl CompositionController::suggestedDestination() const {
    QString folder = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    return QUrl::fromLocalFile(QDir(folder).filePath(
        QStringLiteral("Shadow-%1-%2.tif")
            .arg(mode_, QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss"))
    ));
}
CompositionController::Publication CompositionController::publish(
    QString source,
    QString target,
    std::shared_ptr<std::atomic_bool> cancelled
) {
    if (QFileInfo::exists(target))
        return {{}, "output-conflict"};
    QFile input(source);
    QTemporaryFile output(QFileInfo(target).absolutePath() + "/.shadow-composite-XXXXXX");
    if (!input.open(QIODevice::ReadOnly) || !output.open())
        return {{}, "output-unavailable"};
    while (!input.atEnd()) {
        if (cancelled->load())
            return {{}, "cancelled"};
        auto bytes = input.read(512 * 1024);
        if (bytes.isEmpty() || output.write(bytes) != bytes.size())
            return {{}, "output-unavailable"};
    }
    if (!output.flush())
        return {{}, "output-unavailable"};
    output.close();
    if (cancelled->load())
        return {{}, "cancelled"};
    const auto temporary = nativePath(output.fileName());
    const auto destination = nativePath(target);
#ifdef Q_OS_MACOS
    const bool committed = ::renamex_np(temporary.c_str(), destination.c_str(), RENAME_EXCL) == 0;
#elif defined(Q_OS_WIN)
    const bool committed =
        ::MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH) != 0;
#else
    std::error_code error;
    std::filesystem::create_hard_link(temporary, destination, error);
    const bool committed = !error;
#endif
    if (!committed)
        return {{}, QFileInfo::exists(target) ? "output-conflict" : "output-unavailable"};
    return {
        target,
        {}
    }; // Atomic no-replace commit point. The temporary name is removed by its owner.
}
void CompositionController::save(const QUrl& url) {
    if (!ready() || !staging_)
        return;
    QString target = url.toLocalFile();
    if (target.isEmpty()
        || (!target.endsWith(".tif", Qt::CaseInsensitive)
            && !target.endsWith(".tiff", Qt::CaseInsensitive))) {
        error_ = errorMessage("output-unavailable");
        emit changed();
        return;
    }
    running_ = true;
    phase_ = "saving";
    error_.clear();
    cancelled_->store(false);
    emit changed();
    publication_.setFuture(
        QtConcurrent::run(
            &CompositionController::publish,
            staging_->filePath("result.tif"),
            target,
            cancelled_
        )
    );
}
