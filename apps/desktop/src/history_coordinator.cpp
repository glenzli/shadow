#include "history_coordinator.hpp"

#include <QtConcurrentRun>

#include <stdexcept>
#include <utility>

namespace {

constexpr std::uint32_t HISTORY_PAGE_SIZE = 40;
constexpr std::uint32_t HISTORY_REF_PAGE_SIZE = 64;

[[nodiscard]] LocalizedUiMessage
history_error(const char* const source, const QString& diagnostic) {
    return {
        "HistoryCoordinator",
        source,
        {LocalizedUiArgument(diagnostic)},
    };
}

[[nodiscard]] QVariantMap ref_map(const BackendHistoryRef& reference) {
    QString kind;
    switch (reference.kind) {
    case BackendHistoryRefKind::Working:
        kind = QStringLiteral("working");
        break;
    case BackendHistoryRefKind::Branch:
        kind = QStringLiteral("branch");
        break;
    case BackendHistoryRefKind::NamedVersion:
        kind = QStringLiteral("namedVersion");
        break;
    case BackendHistoryRefKind::Tag:
        kind = QStringLiteral("tag");
        break;
    }
    return {
        {QStringLiteral("name"), reference.name},
        {QStringLiteral("kind"), kind},
        {QStringLiteral("commitId"), reference.commit_id},
        {QStringLiteral("updatedAtMs"), reference.updated_at_ms},
    };
}

} // namespace

HistoryCoordinator::HistoryCoordinator(Operations operations, QObject* parent) :
    QObject(parent), operations_(std::move(operations)), photo_model_(this), library_model_(this) {
    if (!operations_.photo_page || !operations_.library_page || !operations_.library_ref_page) {
        throw std::invalid_argument("complete History operations are required");
    }
    connect(
        &photo_watcher_,
        &QFutureWatcher<PhotoTaskResult>::finished,
        this,
        &HistoryCoordinator::finishPhoto
    );
    connect(
        &library_watcher_,
        &QFutureWatcher<LibraryTaskResult>::finished,
        this,
        &HistoryCoordinator::finishLibrary
    );
    connect(
        &ref_watcher_,
        &QFutureWatcher<RefTaskResult>::finished,
        this,
        &HistoryCoordinator::finishRefs
    );
}

HistoryCoordinator::~HistoryCoordinator() {
    photo_watcher_.waitForFinished();
    library_watcher_.waitForFinished();
    ref_watcher_.waitForFinished();
}

QString HistoryCoordinator::photoId() const {
    return photo_id_;
}

QAbstractItemModel* HistoryCoordinator::photoModel() noexcept {
    return &photo_model_;
}

QAbstractItemModel* HistoryCoordinator::libraryModel() noexcept {
    return &library_model_;
}

QVariantList HistoryCoordinator::libraryRefs() const {
    QVariantList result;
    result.reserve(library_refs_.size());
    for (const auto& reference : library_refs_) {
        result.push_back(ref_map(reference));
    }
    return result;
}

bool HistoryCoordinator::photoBusy() const noexcept {
    return photo_running_;
}

bool HistoryCoordinator::libraryBusy() const noexcept {
    return library_running_;
}

bool HistoryCoordinator::libraryRefsBusy() const noexcept {
    return ref_running_;
}

bool HistoryCoordinator::photoHasMore() const noexcept {
    return photo_has_more_;
}

bool HistoryCoordinator::libraryHasMore() const noexcept {
    return library_has_more_;
}

bool HistoryCoordinator::libraryRefsHaveMore() const noexcept {
    return refs_have_more_;
}

QString HistoryCoordinator::photoErrorText() const {
    return photo_error_.translated();
}

QString HistoryCoordinator::libraryErrorText() const {
    return library_error_.translated();
}

QString HistoryCoordinator::libraryRefsErrorText() const {
    return ref_error_.translated();
}

void HistoryCoordinator::openForPhoto(const QString& photo_id) {
    const QString normalized = photo_id.trimmed();
    if (photo_id_ != normalized) {
        photo_id_ = normalized;
        ++photo_generation_;
        if (photo_generation_ == 0) {
            ++photo_generation_;
        }
        photo_model_.clear();
        photo_cursor_ = {};
        photo_has_more_ = false;
        photo_error_ = {};
        photo_reset_pending_ = photo_running_;
        emit photoChanged();
        emit photoStateChanged();
    }
    refreshPhoto();
    refreshLibrary();
    refreshLibraryRefs();
}

void HistoryCoordinator::refreshPhoto() {
    if (photo_id_.isEmpty()) {
        photo_model_.clear();
        photo_cursor_ = {};
        photo_has_more_ = false;
        photo_error_ = {};
        emit photoStateChanged();
        return;
    }
    startPhoto(true);
}

void HistoryCoordinator::refreshLibrary() {
    startLibrary(true);
}

void HistoryCoordinator::refreshLibraryRefs() {
    startRefs(true);
}

void HistoryCoordinator::refreshAll() {
    refreshPhoto();
    refreshLibrary();
    refreshLibraryRefs();
}

void HistoryCoordinator::loadMorePhoto() {
    if (!photo_running_ && photo_has_more_) {
        startPhoto(false);
    }
}

void HistoryCoordinator::loadMoreLibrary() {
    if (!library_running_ && library_has_more_) {
        startLibrary(false);
    }
}

void HistoryCoordinator::loadMoreLibraryRefs() {
    if (!ref_running_ && refs_have_more_) {
        startRefs(false);
    }
}

void HistoryCoordinator::retranslateUi() {
    emit photoStateChanged();
    emit libraryStateChanged();
    emit libraryRefsChanged();
}

HistoryCoordinator::PhotoTaskResult HistoryCoordinator::runPhotoTask(
    Operations operations,
    QString photo_id,
    BackendHistoryCursor cursor,
    const quint64 generation,
    const quint64 request_id,
    const bool reset
) {
    PhotoTaskResult result{
        .photo_id = photo_id,
        .generation = generation,
        .request_id = request_id,
        .reset = reset,
    };
    try {
        result.page = operations.photo_page(photo_id, cursor, HISTORY_PAGE_SIZE);
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    } catch (...) {
        result.error = QStringLiteral("unknown History backend error");
    }
    return result;
}

HistoryCoordinator::LibraryTaskResult HistoryCoordinator::runLibraryTask(
    Operations operations,
    BackendHistoryCursor cursor,
    const quint64 request_id,
    const bool reset
) {
    LibraryTaskResult result{.request_id = request_id, .reset = reset};
    try {
        result.page = operations.library_page(cursor, HISTORY_PAGE_SIZE);
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    } catch (...) {
        result.error = QStringLiteral("unknown History backend error");
    }
    return result;
}

HistoryCoordinator::RefTaskResult HistoryCoordinator::runRefTask(
    Operations operations,
    QString cursor,
    const quint64 request_id,
    const bool reset
) {
    RefTaskResult result{.request_id = request_id, .reset = reset};
    try {
        result.page = operations.library_ref_page(cursor, HISTORY_REF_PAGE_SIZE);
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    } catch (...) {
        result.error = QStringLiteral("unknown History backend error");
    }
    return result;
}

void HistoryCoordinator::startPhoto(const bool reset) {
    if (photo_running_) {
        photo_reset_pending_ = photo_reset_pending_ || reset;
        return;
    }
    if (photo_id_.isEmpty()) {
        return;
    }
    photo_running_ = true;
    active_photo_request_id_ = ++photo_request_id_;
    photo_error_ = {};
    emit photoStateChanged();
    photo_watcher_.setFuture(
        QtConcurrent::run(
            runPhotoTask,
            operations_,
            photo_id_,
            reset ? BackendHistoryCursor{} : photo_cursor_,
            photo_generation_,
            active_photo_request_id_,
            reset
        )
    );
}

void HistoryCoordinator::startLibrary(const bool reset) {
    if (library_running_) {
        library_reset_pending_ = library_reset_pending_ || reset;
        return;
    }
    library_running_ = true;
    active_library_request_id_ = ++library_request_id_;
    library_error_ = {};
    emit libraryStateChanged();
    library_watcher_.setFuture(
        QtConcurrent::run(
            runLibraryTask,
            operations_,
            reset ? BackendHistoryCursor{} : library_cursor_,
            active_library_request_id_,
            reset
        )
    );
}

void HistoryCoordinator::startRefs(const bool reset) {
    if (ref_running_) {
        ref_reset_pending_ = ref_reset_pending_ || reset;
        return;
    }
    ref_running_ = true;
    active_ref_request_id_ = ++ref_request_id_;
    ref_error_ = {};
    emit libraryRefsChanged();
    ref_watcher_.setFuture(
        QtConcurrent::run(
            runRefTask,
            operations_,
            reset ? QString{} : ref_cursor_,
            active_ref_request_id_,
            reset
        )
    );
}

void HistoryCoordinator::finishPhoto() {
    PhotoTaskResult result = photo_watcher_.result();
    photo_running_ = false;
    const bool accepted = result.request_id == active_photo_request_id_
                          && result.generation == photo_generation_ && result.photo_id == photo_id_;
    if (accepted) {
        if (!result.error.isEmpty()) {
            setPhotoError(result.error);
        } else {
            if (result.reset) {
                photo_model_.replace(std::move(result.page.entries));
            } else {
                photo_model_.append(std::move(result.page.entries));
            }
            photo_has_more_ = result.page.has_more;
            photo_cursor_ = std::move(result.page.next_cursor);
            photo_error_ = {};
        }
    }
    emit photoStateChanged();
    if (photo_reset_pending_) {
        photo_reset_pending_ = false;
        startPhoto(true);
    }
}

void HistoryCoordinator::finishLibrary() {
    LibraryTaskResult result = library_watcher_.result();
    library_running_ = false;
    if (result.request_id == active_library_request_id_) {
        if (!result.error.isEmpty()) {
            setLibraryError(result.error);
        } else {
            if (result.reset) {
                library_model_.replace(std::move(result.page.entries));
            } else {
                library_model_.append(std::move(result.page.entries));
            }
            library_has_more_ = result.page.has_more;
            library_cursor_ = std::move(result.page.next_cursor);
            library_error_ = {};
        }
    }
    emit libraryStateChanged();
    if (library_reset_pending_) {
        library_reset_pending_ = false;
        startLibrary(true);
    }
}

void HistoryCoordinator::finishRefs() {
    RefTaskResult result = ref_watcher_.result();
    ref_running_ = false;
    if (result.request_id == active_ref_request_id_) {
        if (!result.error.isEmpty()) {
            setRefError(result.error);
        } else {
            if (result.reset) {
                library_refs_ = std::move(result.page.refs);
            } else {
                library_refs_.append(std::move(result.page.refs));
            }
            refs_have_more_ = result.page.has_more;
            ref_cursor_ = std::move(result.page.next_cursor);
            ref_error_ = {};
        }
    }
    emit libraryRefsChanged();
    if (ref_reset_pending_) {
        ref_reset_pending_ = false;
        startRefs(true);
    }
}

void HistoryCoordinator::setPhotoError(const QString& diagnostic) {
    photo_error_ = history_error(
        QT_TRANSLATE_NOOP("HistoryCoordinator", "Could not load photo history: %1"),
        diagnostic
    );
}

void HistoryCoordinator::setLibraryError(const QString& diagnostic) {
    library_error_ = history_error(
        QT_TRANSLATE_NOOP("HistoryCoordinator", "Could not load Library history: %1"),
        diagnostic
    );
}

void HistoryCoordinator::setRefError(const QString& diagnostic) {
    ref_error_ = history_error(
        QT_TRANSLATE_NOOP("HistoryCoordinator", "Could not load Library references: %1"),
        diagnostic
    );
}
