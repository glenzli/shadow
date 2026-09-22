#include "people_scope_controller.hpp"

#include "people_analysis_controller.hpp"

#include <QtConcurrentRun>

#include <QSet>

#include <exception>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
using Group = std::pair<QString, QStringList>;

[[nodiscard]] bool accepts(
    const BackendReviewItem& item,
    const PeopleScopeSnapshot& scope,
    const QSet<QString>& semantic,
    const QSet<QString>& smart
) {
    if (scope.only_editable && !item.source_available)
        return false;
    const QString flag =
        item.decision_flag == BackendReviewDecisionFlag::Picked     ? QStringLiteral("picked")
        : item.decision_flag == BackendReviewDecisionFlag::Rejected ? QStringLiteral("rejected")
                                                                    : QStringLiteral("unflagged");
    if (scope.excluded_flag != QStringLiteral("all") && scope.excluded_flag == flag)
        return false;
    if (scope.excluded_color != QStringLiteral("all") && scope.excluded_color == item.color_label)
        return false;
    const QString key = item.photo_id + QChar{0x001f} + item.representation_id;
    return (semantic.isEmpty() || semantic.contains(key))
           && (smart.isEmpty() || smart.contains(key));
}
} // namespace

PeopleScopeController::PeopleScopeController(
    Count count,
    Page page,
    Snapshot snapshot,
    PeopleAnalysisController* people,
    QObject* parent
) :
    QObject(parent), count_(std::move(count)), page_(std::move(page)),
    snapshot_(std::move(snapshot)), people_(people) {
    Q_ASSERT(people_);
    debounce_.setSingleShot(true);
    debounce_.setInterval(80);
    connect(&debounce_, &QTimer::timeout, this, &PeopleScopeController::start);
    connect(
        &watcher_,
        &QFutureWatcher<PeopleScopeResult>::finished,
        this,
        &PeopleScopeController::finish
    );
    connect(
        people_,
        &PeopleAnalysisController::resultsChanged,
        this,
        &PeopleScopeController::invalidate
    );
}

PeopleScopeController::~PeopleScopeController() {
    if (cancel_)
        cancel_->store(true);
    watcher_.waitForFinished();
}

bool PeopleScopeController::active() const noexcept {
    return active_;
}
bool PeopleScopeController::busy() const noexcept {
    return watcher_.isRunning() || debounce_.isActive();
}
bool PeopleScopeController::ready() const noexcept {
    return ready_;
}
QVariantMap PeopleScopeController::counts() const {
    return counts_;
}
QString PeopleScopeController::errorText() const {
    return error_text_;
}

void PeopleScopeController::setViewActive(bool active) {
    if (view_active_ == active)
        return;
    view_active_ = active;
    if (active) {
        invalidate();
    } else {
        debounce_.stop();
        if (cancel_)
            cancel_->store(true);
        ready_ = false;
        emit stateChanged();
    }
}

void PeopleScopeController::invalidate() {
    if (!view_active_)
        return;
    const auto scope = snapshot_();
    active_ = scope.active;
    ready_ = false;
    counts_.clear();
    error_text_.clear();
    dirty_ = true;
    if (cancel_)
        cancel_->store(true);
    if (!watcher_.isRunning())
        debounce_.start();
    emit stateChanged();
}

void PeopleScopeController::start() {
    if (!view_active_ || watcher_.isRunning())
        return;
    dirty_ = false;
    const auto scope = snapshot_();
    active_ = scope.active;
    if (!active_) {
        ready_ = true;
        emit stateChanged();
        return;
    }
    std::vector<Group> groups;
    const QVariantList projected = people_->groups();
    groups.reserve(static_cast<std::size_t>(projected.size()));
    for (const QVariant& value : projected) {
        const QString id = value.toMap().value(QStringLiteral("groupId")).toString();
        const QStringList photos = people_->groupPhotoIds(id);
        if (!id.isEmpty() && !photos.isEmpty())
            groups.emplace_back(id, photos);
    }
    cancel_ = std::make_shared<std::atomic_bool>(false);
    const auto cancel = cancel_;
    const auto count = count_;
    const auto page = page_;
    watcher_.setFuture(
        QtConcurrent::run(
            [scope, groups = std::move(groups), cancel, count, page]() -> PeopleScopeResult {
                PeopleScopeResult result;
                try {
                    const bool needs_rows =
                        scope.only_editable || scope.excluded_flag != QStringLiteral("all")
                        || scope.excluded_color != QStringLiteral("all")
                        || !scope.semantic_keys.isEmpty() || !scope.smart_category_keys.isEmpty();
                    const QSet<QString> semantic(
                        scope.semantic_keys.cbegin(),
                        scope.semantic_keys.cend()
                    );
                    const QSet<QString> smart(
                        scope.smart_category_keys.cbegin(),
                        scope.smart_category_keys.cend()
                    );
                    for (const auto& [id, photos] : groups) {
                        if (cancel->load()) {
                            result.cancelled = true;
                            return result;
                        }
                        BackendLibraryPhotoFilter filter = scope.filter;
                        filter.has_photo_ids = true;
                        filter.photo_ids = photos;
                        std::uint64_t matches = 0;
                        if (!needs_rows) {
                            matches = count(filter);
                        } else {
                            BackendLibraryPhotoCursor cursor;
                            while (true) {
                                if (cancel->load()) {
                                    result.cancelled = true;
                                    return result;
                                }
                                const auto batch = page(filter, cursor, 200);
                                for (const auto& item : batch.items)
                                    if (accepts(item, scope, semantic, smart))
                                        ++matches;
                                if (!batch.has_more)
                                    break;
                                if (batch.items.isEmpty()
                                    || batch.next_cursor.photo_id == cursor.photo_id)
                                    throw std::runtime_error("People scope page did not advance");
                                cursor = batch.next_cursor;
                            }
                        }
                        if (matches > 0)
                            result.counts.insert(id, static_cast<qulonglong>(matches));
                    }
                } catch (const std::exception& error) {
                    result.diagnostic = QString::fromUtf8(error.what());
                } catch (...) {
                    result.diagnostic = QStringLiteral("unknown error");
                }
                return result;
            }
        )
    );
    emit stateChanged();
}

void PeopleScopeController::finish() {
    cancel_.reset();
    if (!view_active_)
        return;
    if (dirty_) {
        debounce_.start();
        emit stateChanged();
        return;
    }
    const PeopleScopeResult result = watcher_.result();
    if (!result.cancelled) {
        counts_ = result.counts;
        error_text_ =
            result.diagnostic.isEmpty() ? QString() : tr("Could not count people in this scope.");
        ready_ = result.diagnostic.isEmpty();
    }
    emit stateChanged();
}
