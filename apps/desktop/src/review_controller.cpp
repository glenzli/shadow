#include "review_controller.hpp"

#include <QtConcurrentRun>

#include <algorithm>
#include <limits>
#include <utility>

namespace {

constexpr std::uint32_t REVIEW_PAGE_SIZE = 96;

[[nodiscard]] ScanTaskResult run_scan(
    const std::shared_ptr<DesktopBackend>& backend,
    const QString& folder_path,
    const quint64 generation
) {
    ScanTaskResult result;
    result.generation = generation;
    try {
        result.report = backend->scanFolder(folder_path);
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

[[nodiscard]] PageTaskResult run_page(
    const std::shared_ptr<DesktopBackend>& backend,
    const QString& cursor_path,
    const QString& cursor_representation_id,
    const quint64 generation,
    const bool reset
) {
    PageTaskResult result;
    result.generation = generation;
    result.reset = reset;
    try {
        result.page = backend->reviewPage(
            cursor_path,
            cursor_representation_id,
            REVIEW_PAGE_SIZE
        );
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

[[nodiscard]] QVector<ReviewItem> review_items(QVector<BackendReviewItem> source) {
    QVector<ReviewItem> items;
    items.reserve(source.size());
    for (auto& item : source) {
        items.push_back({
            .photo_id = std::move(item.photo_id),
            .representation_id = std::move(item.representation_id),
            .title = std::move(item.title),
            .source_path = std::move(item.source_path),
            .visual_role = std::move(item.visual_role),
            .visual_width = item.visual_width,
            .visual_height = item.visual_height,
            .has_visual = item.has_visual,
        });
    }
    return items;
}

[[nodiscard]] int bounded_count(const quint64 count) {
    return static_cast<int>(std::min<quint64>(
        count,
        static_cast<quint64>(std::numeric_limits<int>::max())
    ));
}

} // namespace

ReviewController::ReviewController(
    std::shared_ptr<DesktopBackend> backend,
    QObject* parent
)
    : QObject(parent), backend_(std::move(backend)), model_(this) {
    connect(
        &scan_watcher_,
        &QFutureWatcher<ScanTaskResult>::finished,
        this,
        &ReviewController::finishScan
    );
    connect(
        &page_watcher_,
        &QFutureWatcher<PageTaskResult>::finished,
        this,
        &ReviewController::finishPage
    );
}

ReviewController::~ReviewController() {
    scan_watcher_.waitForFinished();
    page_watcher_.waitForFinished();
}

bool ReviewController::busy() const noexcept {
    return scan_running_ || (page_running_ && model_.rowCount() == 0);
}

bool ReviewController::loadingMore() const noexcept {
    return page_running_ && model_.rowCount() > 0;
}

bool ReviewController::hasMore() const noexcept {
    return has_more_;
}

QString ReviewController::folderPath() const {
    return folder_path_;
}

QString ReviewController::statusText() const {
    return status_text_;
}

int ReviewController::itemCount() const {
    return bounded_count(total_items_);
}

QAbstractItemModel* ReviewController::model() noexcept {
    return &model_;
}

ReviewModel* ReviewController::reviewModel() noexcept {
    return &model_;
}

void ReviewController::scanFolder(const QUrl& folder_url) {
    if (scan_running_ || page_running_) {
        return;
    }
    const QString path = folder_url.toLocalFile();
    if (path.isEmpty()) {
        setStatusText(QStringLiteral("The selected folder is not a local path"));
        return;
    }

    const bool old_busy = busy();
    const bool old_loading_more = loadingMore();
    ++generation_;
    model_.replace({}, generation_);
    total_items_ = 0;
    supported_files_ = 0;
    decode_queued_ = 0;
    issue_count_ = 0;
    next_cursor_path_.clear();
    next_cursor_representation_id_.clear();
    setHasMore(false);
    emit itemCountChanged();
    folder_path_ = path;
    emit folderPathChanged();
    scan_running_ = true;
    emitWorkStateChanges(old_busy, old_loading_more);
    setStatusText(QStringLiteral("Scanning RAW files and preparing Review previews…"));
    scan_watcher_.setFuture(QtConcurrent::run(
        [backend = backend_, folder = folder_path_, generation = generation_]() {
            return run_scan(backend, folder, generation);
        }
    ));
}

void ReviewController::loadMore() {
    if (!has_more_ || scan_running_ || page_running_) {
        return;
    }
    startPage(false);
}

void ReviewController::finishScan() {
    const ScanTaskResult result = scan_watcher_.result();
    const bool old_busy = busy();
    const bool old_loading_more = loadingMore();
    scan_running_ = false;
    emitWorkStateChanges(old_busy, old_loading_more);
    if (result.generation != generation_) {
        return;
    }
    if (!result.error.isEmpty()) {
        setStatusText(QStringLiteral("Scan failed · %1").arg(result.error));
        return;
    }
    folder_path_ = result.report.folder_path;
    supported_files_ = result.report.supported_files;
    decode_queued_ = result.report.decode_queued;
    issue_count_ = result.report.issue_count;
    emit folderPathChanged();
    startPage(true);
}

void ReviewController::finishPage() {
    PageTaskResult result = page_watcher_.result();
    const bool old_busy = busy();
    const bool old_loading_more = loadingMore();
    page_running_ = false;
    emitWorkStateChanges(old_busy, old_loading_more);
    if (result.generation != generation_) {
        return;
    }
    if (!result.error.isEmpty()) {
        setStatusText(QStringLiteral("Review page failed · %1").arg(result.error));
        return;
    }

    total_items_ = result.page.total_items;
    next_cursor_path_ = std::move(result.page.next_cursor_path);
    next_cursor_representation_id_ =
        std::move(result.page.next_cursor_representation_id);
    setHasMore(result.page.has_more);
    auto items = review_items(std::move(result.page.items));
    if (result.reset) {
        model_.replace(std::move(items), generation_);
    } else {
        model_.append(std::move(items));
    }
    emit itemCountChanged();
    updateReadyStatus();
}

void ReviewController::startPage(const bool reset) {
    if (page_running_) {
        return;
    }
    const bool old_busy = busy();
    const bool old_loading_more = loadingMore();
    page_running_ = true;
    emitWorkStateChanges(old_busy, old_loading_more);
    if (reset) {
        setStatusText(QStringLiteral("Loading the first Review page…"));
    } else {
        updateReadyStatus();
    }
    page_watcher_.setFuture(QtConcurrent::run([
        backend = backend_,
        cursor_path = reset ? QString{} : next_cursor_path_,
        cursor_id = reset ? QString{} : next_cursor_representation_id_,
        generation = generation_,
        reset
    ]() { return run_page(backend, cursor_path, cursor_id, generation, reset); }));
}

void ReviewController::emitWorkStateChanges(
    const bool old_busy,
    const bool old_loading_more
) {
    if (old_busy != busy()) {
        emit busyChanged();
    }
    if (old_loading_more != loadingMore()) {
        emit loadingMoreChanged();
    }
}

void ReviewController::setHasMore(const bool has_more) {
    if (has_more_ == has_more) {
        return;
    }
    has_more_ = has_more;
    emit hasMoreChanged();
}

void ReviewController::setStatusText(QString status) {
    if (status_text_ == status) {
        return;
    }
    status_text_ = std::move(status);
    emit statusTextChanged();
}

void ReviewController::updateReadyStatus() {
    const QString loading = loadingMore() ? QStringLiteral(" · loading more") : QString{};
    setStatusText(
        QStringLiteral("%1 / %2 loaded · %3 supported · %4 rebuilt · %5 issues%6")
            .arg(model_.rowCount())
            .arg(total_items_)
            .arg(supported_files_)
            .arg(decode_queued_)
            .arg(issue_count_)
            .arg(loading)
    );
}
