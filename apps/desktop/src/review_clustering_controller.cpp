#include "review_clustering_controller.hpp"

#include <QtConcurrentRun>

#include <QHash>
#include <QMetaObject>
#include <QSet>
#include <QVariantMap>

#include <algorithm>
#include <exception>
#include <numeric>
#include <utility>

namespace {
constexpr int maximum_scope_photos = 240;
constexpr int maximum_group_photos = 12;
constexpr int candidate_neighbors = 3;
} // namespace

ReviewClusteringController::ReviewClusteringController(
    ImageRunner image_runner,
    Begin begin,
    Cancel cancel,
    AlbumPage album_page,
    QObject* parent
) :
    QObject(parent), image_runner_(std::move(image_runner)), begin_(std::move(begin)),
    cancel_(std::move(cancel)), album_page_(std::move(album_page)) {
    connect(
        &watcher_,
        &QFutureWatcher<ReviewClusteringResult>::finished,
        this,
        &ReviewClusteringController::finish
    );
}

ReviewClusteringController::~ReviewClusteringController() {
    cancel_requested_.store(true);
    pause_condition_.notify_all();
    const auto token = active_token_.exchange(0);
    if (token && cancel_)
        cancel_(token);
    watcher_.waitForFinished();
}

bool ReviewClusteringController::busy() const noexcept {
    return state_ == State::Collecting || state_ == State::Running || state_ == State::Pausing
           || state_ == State::Paused || state_ == State::Cancelling;
}
bool ReviewClusteringController::paused() const noexcept {
    return state_ == State::Paused;
}
bool ReviewClusteringController::canResume() const noexcept {
    return state_ == State::Paused || state_ == State::Pausing;
}
bool ReviewClusteringController::hasResults() const noexcept {
    return state_ == State::Ready && !groups_.isEmpty();
}
bool ReviewClusteringController::hasStatus() const noexcept {
    return state_ != State::Idle;
}
bool ReviewClusteringController::cancelling() const noexcept {
    return state_ == State::Cancelling;
}
int ReviewClusteringController::processed() const noexcept {
    return processed_;
}
int ReviewClusteringController::total() const noexcept {
    return total_;
}

QString ReviewClusteringController::statusText() const {
    switch (state_) {
    case State::Idle:
        return tr("Choose photos or an album to group similar review candidates.");
    case State::Invalid:
        return tr("Choose at least two local photos for candidate grouping.");
    case State::Collecting:
        return tr("Collecting album photos…");
    case State::Running:
        return tr("Grouping candidates · %L1 of %L2 photos checked").arg(processed_).arg(total_);
    case State::Pausing:
        return tr("Pausing after the current photo…");
    case State::Paused:
        return tr("Paused · %L1 of %L2 photos checked").arg(processed_).arg(total_);
    case State::Cancelling:
        return tr("Cancelling the current photo…");
    case State::Ready:
        return truncated_ ? tr("%L1 candidate groups ready from the first %L2 photos.")
                                .arg(groups_.size())
                                .arg(total_)
                          : tr("%L1 candidate groups ready from %L2 photos.")
                                .arg(groups_.size())
                                .arg(total_);
    case State::Cancelled:
        return tr("Candidate grouping was cancelled.");
    case State::Failed:
        return tr("Candidate grouping could not finish. Check Infer Runtime and try again.");
    }
    return {};
}

QVariantList ReviewClusteringController::groups() const {
    QVariantList projected;
    projected.reserve(groups_.size());
    for (const QStringList& keys : groups_)
        projected.push_back(QVariantMap{{QStringLiteral("representationKeys"), keys}});
    return projected;
}

QString ReviewClusteringController::key(const Ticket& ticket) {
    return ticket.photo_id + QChar{0x001f} + ticket.representation_id;
}

bool ReviewClusteringController::startSelected(const QVariantList& targets) {
    QVector<Ticket> tickets;
    tickets.reserve(std::min<qsizetype>(targets.size(), maximum_scope_photos + 1));
    for (const QVariant& value : targets) {
        if (tickets.size() > maximum_scope_photos)
            break;
        const QVariantMap target = value.toMap();
        if (target.value(QStringLiteral("isRemote")).toBool())
            continue;
        tickets.push_back(
            {target.value(QStringLiteral("photoId")).toString(),
             target.value(QStringLiteral("representationId")).toString()}
        );
    }
    return start(std::move(tickets), {});
}

bool ReviewClusteringController::startView(QAbstractItemModel* model) {
    if (!model)
        return false;
    const auto roles = model->roleNames();
    int photo_role = -1;
    int representation_role = -1;
    int remote_role = -1;
    for (auto it = roles.cbegin(); it != roles.cend(); ++it) {
        if (it.value() == "photoId")
            photo_role = it.key();
        if (it.value() == "representationId")
            representation_role = it.key();
        if (it.value() == "isRemote")
            remote_role = it.key();
    }
    if (photo_role < 0 || representation_role < 0)
        return false;
    QVector<Ticket> tickets;
    const int count = std::min(model->rowCount(), maximum_scope_photos + 1);
    tickets.reserve(count);
    for (int row = 0; row < count; ++row) {
        const QModelIndex index = model->index(row, 0);
        if (remote_role >= 0 && model->data(index, remote_role).toBool())
            continue;
        tickets.push_back(
            {model->data(index, photo_role).toString(),
             model->data(index, representation_role).toString()}
        );
    }
    return start(std::move(tickets), {});
}

bool ReviewClusteringController::startAlbum(const QString& album_id) {
    if (album_id.isEmpty() || !album_page_)
        return false;
    return start({}, album_id);
}

bool ReviewClusteringController::start(QVector<Ticket> tickets, const QString& album_id) {
    if (busy() || !image_runner_ || !begin_ || !cancel_)
        return false;
    if (album_id.isEmpty() && tickets.size() < 2) {
        state_ = State::Invalid;
        emit stateChanged();
        return false;
    }
    cancel_requested_.store(false);
    pause_requested_.store(false);
    active_token_.store(0);
    discard_result_ = false;
    groups_.clear();
    processed_ = 0;
    total_ = album_id.isEmpty()
                 ? static_cast<int>(std::min<qsizetype>(tickets.size(), maximum_scope_photos))
                 : 0;
    truncated_ = tickets.size() > maximum_scope_photos;
    state_ = album_id.isEmpty() ? State::Running : State::Collecting;
    const auto generation = ++generation_;
    watcher_.setFuture(
        QtConcurrent::run([this, tickets = std::move(tickets), album_id, generation]() mutable {
            return run(std::move(tickets), album_id, generation);
        })
    );
    emit stateChanged();
    return true;
}

void ReviewClusteringController::pause() {
    if (state_ != State::Running && state_ != State::Collecting)
        return;
    pause_requested_.store(true);
    state_ = State::Pausing;
    emit stateChanged();
}

void ReviewClusteringController::resume() {
    if (!canResume())
        return;
    pause_requested_.store(false);
    pause_condition_.notify_all();
    state_ = total_ > 0 ? State::Running : State::Collecting;
    emit stateChanged();
}

void ReviewClusteringController::cancel() {
    if (!busy() || state_ == State::Cancelling)
        return;
    cancel_requested_.store(true);
    pause_condition_.notify_all();
    const auto token = active_token_.exchange(0);
    if (token)
        cancel_(token);
    state_ = State::Cancelling;
    emit stateChanged();
}

void ReviewClusteringController::clear() {
    if (busy()) {
        discard_result_ = true;
        cancel();
    } else {
        groups_.clear();
        state_ = State::Idle;
        processed_ = total_ = 0;
        emit stateChanged();
    }
}

void ReviewClusteringController::retranslateUi() {
    emit stateChanged();
}

bool ReviewClusteringController::waitIfPaused(const std::uint64_t generation) {
    if (cancel_requested_.load())
        return false;
    if (!pause_requested_.load())
        return true;
    QMetaObject::invokeMethod(
        this,
        [this, generation] {
            if (generation == generation_ && state_ == State::Pausing) {
                state_ = State::Paused;
                emit stateChanged();
            }
        },
        Qt::QueuedConnection
    );
    std::unique_lock lock(pause_mutex_);
    pause_condition_.wait(lock, [this] {
        return cancel_requested_.load() || !pause_requested_.load();
    });
    return !cancel_requested_.load();
}

void ReviewClusteringController::publishProgress(
    const std::uint64_t generation,
    const int processed,
    const int total
) {
    QMetaObject::invokeMethod(
        this,
        [this, generation, processed, total] {
            if (generation != generation_ || state_ == State::Cancelling)
                return;
            processed_ = processed;
            total_ = total;
            if (state_ == State::Collecting)
                state_ = State::Running;
            emit stateChanged();
        },
        Qt::QueuedConnection
    );
}

ReviewClusteringResult ReviewClusteringController::run(
    QVector<Ticket> tickets,
    const QString& album_id,
    const std::uint64_t generation
) {
    ReviewClusteringResult result;
    try {
        if (!album_id.isEmpty()) {
            BackendLibraryPhotoCursor cursor;
            for (;;) {
                if (!waitIfPaused(generation)) {
                    result.cancelled = true;
                    return result;
                }
                const auto page = album_page_(album_id, cursor, 64);
                for (const auto& item : page.items)
                    if (item.has_visual)
                        tickets.push_back({item.photo_id, item.representation_id});
                if (tickets.size() > maximum_scope_photos) {
                    result.truncated = true;
                    break;
                }
                if (!page.has_more || page.items.isEmpty())
                    break;
                cursor = page.next_cursor;
            }
        }
        result.truncated |= tickets.size() > maximum_scope_photos;
        tickets.resize(std::min<qsizetype>(tickets.size(), maximum_scope_photos));
        QSet<QString> seen;
        QVector<Ticket> unique;
        for (const Ticket& ticket : tickets) {
            const QString identity = key(ticket);
            if (ticket.photo_id.isEmpty() || ticket.representation_id.isEmpty()
                || seen.contains(identity))
                continue;
            seen.insert(identity);
            unique.push_back(ticket);
        }
        tickets = std::move(unique);
        result.total = static_cast<int>(tickets.size());
        publishProgress(generation, 0, result.total);
        if (tickets.size() < 2)
            return result;

        QHash<QString, int> indices;
        for (int index = 0; index < tickets.size(); ++index)
            indices.insert(key(tickets[index]), index);
        QVector<QSet<int>> neighbors(tickets.size());
        for (int index = 0; index < tickets.size(); ++index) {
            if (!waitIfPaused(generation)) {
                result.cancelled = true;
                return result;
            }
            const auto token = begin_();
            active_token_.store(token);
            if (cancel_requested_.load()) {
                cancel_(token);
                active_token_.store(0);
                result.cancelled = true;
                return result;
            }
            BackendSemanticSearchReport report;
            try {
                report =
                    image_runner_(tickets[index].photo_id, tickets[index].representation_id, token);
            } catch (const std::exception& error) {
                active_token_.store(0);
                if (cancel_requested_.load())
                    result.cancelled = true;
                else
                    result.diagnostic = QString::fromUtf8(error.what());
                return result;
            }
            active_token_.store(0);
            if (cancel_requested_.load()) {
                result.cancelled = true;
                return result;
            }
            for (const auto& match : report.matches) {
                const auto found =
                    indices.constFind(match.photo_id + QChar{0x001f} + match.representation_id);
                if (found == indices.cend() || *found == index)
                    continue;
                neighbors[index].insert(*found);
                if (neighbors[index].size() == candidate_neighbors)
                    break;
            }
            publishProgress(generation, index + 1, result.total);
        }

        QVector<int> parent(tickets.size());
        std::iota(parent.begin(), parent.end(), 0);
        auto root = [&parent](int index) {
            while (parent[index] != index) {
                parent[index] = parent[parent[index]];
                index = parent[index];
            }
            return index;
        };
        for (int index = 0; index < tickets.size(); ++index)
            for (int neighbor : neighbors[index])
                if (neighbors[neighbor].contains(index))
                    parent[root(neighbor)] = root(index);
        QHash<int, QVector<int>> components;
        for (int index = 0; index < tickets.size(); ++index)
            components[root(index)].push_back(index);
        QVector<QVector<int>> ordered;
        for (auto it = components.cbegin(); it != components.cend(); ++it)
            if (it.value().size() > 1)
                ordered.push_back(it.value());
        std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
            return a.front() < b.front();
        });
        for (const auto& component : ordered) {
            for (int offset = 0; offset < component.size(); offset += maximum_group_photos) {
                const int end = static_cast<int>(
                    std::min<qsizetype>(component.size(), offset + maximum_group_photos)
                );
                if (end - offset < 2)
                    break;
                QStringList keys;
                for (int position = offset; position < end; ++position)
                    keys.push_back(key(tickets[component[position]]));
                result.groups.push_back(keys);
            }
        }
    } catch (const std::exception& error) {
        if (cancel_requested_.load())
            result.cancelled = true;
        else
            result.diagnostic = QString::fromUtf8(error.what());
    }
    return result;
}

void ReviewClusteringController::finish() {
    const auto result = watcher_.result();
    active_token_.store(0);
    if (discard_result_) {
        groups_.clear();
        processed_ = total_ = 0;
        state_ = State::Idle;
    } else if (result.cancelled) {
        groups_.clear();
        state_ = State::Cancelled;
    } else if (!result.diagnostic.isEmpty()) {
        groups_.clear();
        state_ = State::Failed;
    } else {
        groups_ = result.groups;
        total_ = result.total;
        processed_ = result.total;
        truncated_ = result.truncated;
        state_ = State::Ready;
    }
    emit stateChanged();
}
