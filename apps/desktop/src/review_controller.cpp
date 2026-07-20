#include "review_controller.hpp"

#include "shadow-desktop-bridge/src/lib.rs.h"

#include <QtConcurrentRun>

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

namespace {

[[nodiscard]] QString qstring(const rust::String& value) {
    const auto length = std::min<std::size_t>(
        value.size(),
        static_cast<std::size_t>(std::numeric_limits<qsizetype>::max())
    );
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(length));
}

[[nodiscard]] QByteArray qbytes(const rust::Vec<std::uint8_t>& value) {
    const auto length = std::min<std::size_t>(
        value.size(),
        static_cast<std::size_t>(std::numeric_limits<qsizetype>::max())
    );
    return QByteArray(
        reinterpret_cast<const char*>(value.data()),
        static_cast<qsizetype>(length)
    );
}

[[nodiscard]] ScanTaskResult run_scan(
    const QString& catalog_path,
    const QString& cache_root,
    const QString& folder_path
) {
    ScanTaskResult result;
    try {
        const std::string catalog = catalog_path.toStdString();
        const std::string cache = cache_root.toStdString();
        const std::string folder = folder_path.toStdString();
        auto snapshot = shadow::desktop::scan_review(catalog, cache, folder);
        result.folder_path = qstring(snapshot.folder_path);
        result.files_seen = snapshot.files_seen;
        result.supported_files = snapshot.supported_files;
        result.decode_queued = snapshot.decode_inspections_queued;
        result.issue_count = snapshot.issue_count;
        result.items.reserve(static_cast<qsizetype>(snapshot.items.size()));
        for (const auto& source : snapshot.items) {
            ReviewItem item;
            item.photo_id = qstring(source.photo_id);
            item.representation_id = qstring(source.representation_id);
            item.title = qstring(source.title);
            item.source_path = qstring(source.source_path);
            item.visual_role = qstring(source.visual_role);
            item.visual_error = qstring(source.visual_error);
            item.visual_width = source.visual_width;
            item.visual_height = source.visual_height;
            item.visual_bytes = qbytes(source.visual_bytes);
            result.items.push_back(std::move(item));
        }
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

} // namespace

ReviewController::ReviewController(
    QString catalog_path,
    QString cache_root,
    QObject* parent
)
    : QObject(parent),
      catalog_path_(std::move(catalog_path)),
      cache_root_(std::move(cache_root)),
      model_(this) {
    connect(&watcher_, &QFutureWatcher<ScanTaskResult>::finished, this, &ReviewController::finishScan);
}

ReviewController::~ReviewController() {
    watcher_.waitForFinished();
}

bool ReviewController::busy() const noexcept {
    return busy_;
}

QString ReviewController::folderPath() const {
    return folder_path_;
}

QString ReviewController::statusText() const {
    return status_text_;
}

int ReviewController::itemCount() const {
    return model_.rowCount();
}

QAbstractItemModel* ReviewController::model() noexcept {
    return &model_;
}

ReviewModel* ReviewController::reviewModel() noexcept {
    return &model_;
}

void ReviewController::scanFolder(const QUrl& folder_url) {
    if (busy_) {
        return;
    }
    const QString path = folder_url.toLocalFile();
    if (path.isEmpty()) {
        setStatusText(QStringLiteral("The selected folder is not a local path"));
        return;
    }
    folder_path_ = path;
    emit folderPathChanged();
    setBusy(true);
    setStatusText(QStringLiteral("Scanning RAW files and preparing Review previews…"));
    watcher_.setFuture(QtConcurrent::run(
        [catalog = catalog_path_, cache = cache_root_, folder = folder_path_]() {
            return run_scan(catalog, cache, folder);
        }
    ));
}

void ReviewController::finishScan() {
    ScanTaskResult result = watcher_.result();
    setBusy(false);
    if (!result.error.isEmpty()) {
        setStatusText(QStringLiteral("Scan failed · %1").arg(result.error));
        return;
    }
    folder_path_ = result.folder_path;
    emit folderPathChanged();
    model_.replace(std::move(result.items));
    emit itemCountChanged();
    setStatusText(
        QStringLiteral("%1 photos · %2 supported · %3 rebuilt · %4 issues")
            .arg(model_.rowCount())
            .arg(result.supported_files)
            .arg(result.decode_queued)
            .arg(result.issue_count)
    );
}

void ReviewController::setBusy(const bool busy) {
    if (busy_ == busy) {
        return;
    }
    busy_ = busy;
    emit busyChanged();
}

void ReviewController::setStatusText(QString status) {
    if (status_text_ == status) {
        return;
    }
    status_text_ = std::move(status);
    emit statusTextChanged();
}
