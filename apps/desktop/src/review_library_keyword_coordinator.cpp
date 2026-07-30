#include "review_library_keyword_coordinator.hpp"

#include <QSet>
#include <QtConcurrentRun>

#include <initializer_list>
#include <stdexcept>
#include <utility>

namespace {

[[nodiscard]] LocalizedUiMessage keyword_message(
    const char* const source,
    const std::initializer_list<LocalizedUiArgument> arguments = {}
) {
    return {"ReviewController", source, arguments};
}

[[nodiscard]] QVariantMap keyword_map(const BackendLibraryKeyword& keyword) {
    return {
        {QStringLiteral("id"), keyword.id},
        {QStringLiteral("parentId"), keyword.parent_id},
        {QStringLiteral("name"), keyword.name},
        {QStringLiteral("depth"), keyword.depth},
        {
            QStringLiteral("photoCount"),
            QVariant::fromValue<qulonglong>(keyword.subtree_photo_count),
        },
    };
}

[[nodiscard]] QString origin_name(const BackendLibraryKeywordOrigin origin) {
    switch (origin) {
    case BackendLibraryKeywordOrigin::Manual:
        return QStringLiteral("manual");
    case BackendLibraryKeywordOrigin::Imported:
        return QStringLiteral("imported");
    case BackendLibraryKeywordOrigin::AiAccepted:
        return QStringLiteral("ai-accepted");
    }
    return {};
}

} // namespace

ReviewLibraryKeywordCoordinator::ReviewLibraryKeywordCoordinator(
    Operations operations,
    QObject* parent
) : QObject(parent), operations_(std::move(operations)) {
    if (!operations_.tree || !operations_.for_photo || !operations_.create || !operations_.rename
        || !operations_.move || !operations_.remove || !operations_.assign
        || !operations_.unassign) {
        throw std::invalid_argument("complete Review Library keyword operations are required");
    }
    connect(
        &watcher_,
        &QFutureWatcher<TaskResult>::finished,
        this,
        &ReviewLibraryKeywordCoordinator::finishTask
    );
}

ReviewLibraryKeywordCoordinator::~ReviewLibraryKeywordCoordinator() {
    watcher_.waitForFinished();
}

QVariantList ReviewLibraryKeywordCoordinator::keywords() const {
    QVariantList result;
    result.reserve(keywords_.size());
    for (const auto& keyword : keywords_) {
        result.push_back(keyword_map(keyword));
    }
    return result;
}

QVariantList ReviewLibraryKeywordCoordinator::photoKeywords() const {
    QVariantList result;
    result.reserve(photo_keywords_.size());
    for (const auto& assignment : photo_keywords_) {
        QVariantMap value = keyword_map(assignment.keyword);
        value.insert(QStringLiteral("origin"), origin_name(assignment.origin));
        value.insert(QStringLiteral("sourceLabel"), assignment.source_label);
        value.insert(QStringLiteral("hasConfidence"), assignment.has_confidence);
        value.insert(
            QStringLiteral("confidence"),
            assignment.has_confidence ? static_cast<double>(assignment.confidence_milli) / 1000.0
                                      : 0.0
        );
        result.push_back(std::move(value));
    }
    return result;
}

QString ReviewLibraryKeywordCoordinator::photoId() const {
    return photo_id_;
}

bool ReviewLibraryKeywordCoordinator::busy() const noexcept {
    return task_running_;
}

LocalizedUiMessage ReviewLibraryKeywordCoordinator::statusMessage() const {
    return status_message_;
}

void ReviewLibraryKeywordCoordinator::setPhotoId(const QString& photo_id) {
    const QString normalized = photo_id.trimmed();
    if (photo_id_ == normalized) {
        return;
    }
    photo_id_ = normalized;
    photo_keywords_.clear();
    emit photoKeywordsChanged();
    refresh();
}

void ReviewLibraryKeywordCoordinator::refresh() {
    if (task_running_) {
        refresh_pending_ = true;
        return;
    }
    startTask(TaskAction::Refresh);
}

void ReviewLibraryKeywordCoordinator::createKeyword(const QString& parent_id, const QString& name) {
    const QString normalized_name = name.trimmed();
    if (task_running_ || normalized_name.isEmpty()) {
        return;
    }
    startTask(TaskAction::Create, {}, parent_id.trimmed(), normalized_name);
}

void ReviewLibraryKeywordCoordinator::renameKeyword(
    const QString& keyword_id,
    const QString& name
) {
    const QString normalized_id = keyword_id.trimmed();
    const QString normalized_name = name.trimmed();
    if (task_running_ || normalized_id.isEmpty() || normalized_name.isEmpty()) {
        return;
    }
    startTask(TaskAction::Rename, normalized_id, {}, normalized_name);
}

void ReviewLibraryKeywordCoordinator::moveKeyword(
    const QString& keyword_id,
    const QString& parent_id
) {
    const QString normalized_id = keyword_id.trimmed();
    if (task_running_ || normalized_id.isEmpty()) {
        return;
    }
    startTask(TaskAction::Move, normalized_id, parent_id.trimmed());
}

void ReviewLibraryKeywordCoordinator::deleteKeyword(const QString& keyword_id) {
    const QString normalized_id = keyword_id.trimmed();
    if (task_running_ || normalized_id.isEmpty()) {
        return;
    }
    startTask(TaskAction::Delete, normalized_id);
}

void ReviewLibraryKeywordCoordinator::assignKeyword(
    const QString& keyword_id,
    const QVariantList& targets
) {
    const QString normalized_id = keyword_id.trimmed();
    const QStringList photo_ids = photoIds(targets);
    if (task_running_ || normalized_id.isEmpty() || photo_ids.isEmpty()) {
        return;
    }
    startTask(TaskAction::Assign, normalized_id, {}, {}, photo_ids);
}

void ReviewLibraryKeywordCoordinator::removeKeyword(
    const QString& keyword_id,
    const QVariantList& targets
) {
    const QString normalized_id = keyword_id.trimmed();
    const QStringList photo_ids = photoIds(targets);
    if (task_running_ || normalized_id.isEmpty() || photo_ids.isEmpty()) {
        return;
    }
    startTask(TaskAction::Unassign, normalized_id, {}, {}, photo_ids);
}

void ReviewLibraryKeywordCoordinator::retranslateUi() {
    if (!status_message_.isEmpty()) {
        emit statusMessageChanged();
    }
}

QStringList ReviewLibraryKeywordCoordinator::photoIds(const QVariantList& targets) {
    QStringList photo_ids;
    QSet<QString> seen;
    for (const QVariant& target : targets) {
        const QString photo_id =
            target.toMap().value(QStringLiteral("photoId")).toString().trimmed();
        if (!photo_id.isEmpty() && !seen.contains(photo_id)) {
            seen.insert(photo_id);
            photo_ids.push_back(photo_id);
        }
    }
    return photo_ids;
}

ReviewLibraryKeywordCoordinator::TaskResult ReviewLibraryKeywordCoordinator::runTask(
    Operations operations,
    const TaskAction action,
    QString keyword_id,
    QString parent_id,
    QString name,
    QStringList photo_ids,
    QString selected_photo_id,
    const quint64 request_id
) {
    TaskResult result;
    result.request_id = request_id;
    result.action = action;
    result.selected_photo_id = selected_photo_id;
    try {
        switch (action) {
        case TaskAction::Refresh:
            break;
        case TaskAction::Create:
            operations.create(parent_id, name);
            break;
        case TaskAction::Rename:
            operations.rename(keyword_id, name);
            break;
        case TaskAction::Move:
            operations.move(keyword_id, parent_id);
            break;
        case TaskAction::Delete: {
            const auto receipt = operations.remove(keyword_id);
            result.deleted_keyword_count = receipt.deleted_keyword_count;
            break;
        }
        case TaskAction::Assign: {
            const auto receipt = operations.assign(keyword_id, photo_ids);
            result.changed_photo_count = receipt.changed_photo_count;
            break;
        }
        case TaskAction::Unassign: {
            const auto receipt = operations.unassign(keyword_id, photo_ids);
            result.changed_photo_count = receipt.changed_photo_count;
            break;
        }
        }
        result.keywords = operations.tree();
        if (!selected_photo_id.isEmpty()) {
            result.photo_keywords = operations.for_photo(selected_photo_id);
        }
        result.has_snapshot = true;
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
        try {
            result.keywords = operations.tree();
            if (!selected_photo_id.isEmpty()) {
                result.photo_keywords = operations.for_photo(selected_photo_id);
            }
            result.has_snapshot = true;
        } catch (const std::exception&) {
            // Preserve the primary failure.
        }
    }
    return result;
}

void ReviewLibraryKeywordCoordinator::startTask(
    const TaskAction action,
    const QString& keyword_id,
    const QString& parent_id,
    const QString& name,
    const QStringList& photo_ids
) {
    task_running_ = true;
    active_request_id_ = ++request_id_;
    if (action != TaskAction::Refresh) {
        setStatus(
            keyword_message(QT_TRANSLATE_NOOP("ReviewController", "Updating Library keywords…"))
        );
    }
    emit keywordsChanged();
    watcher_.setFuture(
        QtConcurrent::run(
            runTask,
            operations_,
            action,
            keyword_id,
            parent_id,
            name,
            photo_ids,
            photo_id_,
            active_request_id_
        )
    );
}

void ReviewLibraryKeywordCoordinator::finishTask() {
    TaskResult result = watcher_.result();
    task_running_ = false;
    const bool request_is_current = result.request_id == active_request_id_;
    const bool selection_is_current = result.selected_photo_id == photo_id_;
    if (request_is_current && selection_is_current && result.has_snapshot) {
        keywords_ = std::move(result.keywords);
        photo_keywords_ = std::move(result.photo_keywords);
    }
    if (request_is_current && result.error.isEmpty()) {
        publishSuccess(result);
        if (result.action != TaskAction::Refresh) {
            emit keywordMutationAccepted();
        }
    } else if (request_is_current) {
        setStatus(keyword_message(
            QT_TRANSLATE_NOOP("ReviewController", "Could not update Library keywords · %1"),
            {result.error}
        ));
    }
    emit keywordsChanged();
    emit photoKeywordsChanged();
    if (refresh_pending_ || !selection_is_current) {
        refresh_pending_ = false;
        startTask(TaskAction::Refresh);
    }
}

void ReviewLibraryKeywordCoordinator::publishSuccess(const TaskResult& result) {
    switch (result.action) {
    case TaskAction::Refresh:
        return;
    case TaskAction::Create:
        setStatus(
            keyword_message(QT_TRANSLATE_NOOP("ReviewController", "Library keyword created"))
        );
        return;
    case TaskAction::Rename:
        setStatus(
            keyword_message(QT_TRANSLATE_NOOP("ReviewController", "Library keyword renamed"))
        );
        return;
    case TaskAction::Move:
        setStatus(keyword_message(QT_TRANSLATE_NOOP("ReviewController", "Library keyword moved")));
        return;
    case TaskAction::Delete:
        setStatus(keyword_message(
            QT_TRANSLATE_NOOP("ReviewController", "Deleted keyword nodes · %1"),
            {QString::number(result.deleted_keyword_count)}
        ));
        return;
    case TaskAction::Assign:
        setStatus(keyword_message(
            QT_TRANSLATE_NOOP("ReviewController", "Tagged selected photos · %1 changed"),
            {QString::number(result.changed_photo_count)}
        ));
        return;
    case TaskAction::Unassign:
        setStatus(keyword_message(
            QT_TRANSLATE_NOOP("ReviewController", "Removed keyword assignments · %1 changed"),
            {QString::number(result.changed_photo_count)}
        ));
        return;
    }
}

void ReviewLibraryKeywordCoordinator::setStatus(LocalizedUiMessage status) {
    status_message_ = std::move(status);
    emit statusMessageChanged();
}
