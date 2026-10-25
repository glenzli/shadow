#include "shared_grade_link_controller.hpp"

#include <QCoreApplication>
#include <QEvent>
#include <QFileInfo>
#include <QTimer>
#include <QtConcurrent>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace {
constexpr int page_size = 64;
constexpr int result_limit = 200;
constexpr int retry_limit = 1000;

const BackendGradeNode* source_node(const SharedGradeLinkSource& source) {
    const auto& nodes = source.state.grade_stack.grade_nodes;
    const auto found = std::find_if(nodes.cbegin(), nodes.cend(), [&](const auto& node) {
        return node.grade_node_id == source.grade_node_id;
    });
    return found == nodes.cend() ? nullptr : &*found;
}

QString result_key(const SharedGradeLinkTarget& target, const BackendSharedGradeNode* shared) {
    return target.photo.photo_id + QLatin1Char('/') + (shared ? shared->layer_id : QString{});
}
LocalizedUiMessage issue_message(BackendSharedGradeLinkStatus issue) {
    switch (issue) {
    case BackendSharedGradeLinkStatus::Ready:
        return {
            "SharedGradeLinkController",
            QT_TRANSLATE_NOOP("SharedGradeLinkController", "Waiting")
        };
    case BackendSharedGradeLinkStatus::Updated:
        return {
            "SharedGradeLinkController",
            QT_TRANSLATE_NOOP("SharedGradeLinkController", "Updated")
        };
    case BackendSharedGradeLinkStatus::Unchanged:
        return {
            "SharedGradeLinkController",
            QT_TRANSLATE_NOOP("SharedGradeLinkController", "Already current")
        };
    case BackendSharedGradeLinkStatus::LocalChanged:
        return {
            "SharedGradeLinkController",
            QT_TRANSLATE_NOOP(
                "SharedGradeLinkController",
                "Local changes kept · publish them or unlink this node"
            )
        };
    case BackendSharedGradeLinkStatus::HeadChanged:
        return {
            "SharedGradeLinkController",
            QT_TRANSLATE_NOOP(
                "SharedGradeLinkController",
                "The photo or shared revision changed · newer edits kept"
            )
        };
    case BackendSharedGradeLinkStatus::Unavailable:
        return {
            "SharedGradeLinkController",
            QT_TRANSLATE_NOOP(
                "SharedGradeLinkController",
                "Source unavailable · previous edits kept"
            )
        };
    case BackendSharedGradeLinkStatus::NodeLimit:
        return {
            "SharedGradeLinkController",
            QT_TRANSLATE_NOOP(
                "SharedGradeLinkController",
                "Not enough room for another adjustment node"
            )
        };
    case BackendSharedGradeLinkStatus::Failed:
        return {
            "SharedGradeLinkController",
            QT_TRANSLATE_NOOP("SharedGradeLinkController", "Could not update · previous edits kept")
        };
    }
    return {};
}
} // namespace

SharedGradeLinkController::SharedGradeLinkController(
    SharedGradeLinkOperations operations,
    SharedGradeLinkEditor editor,
    QObject* parent
) : QObject(parent), operations_(std::move(operations)), editor_(std::move(editor)) {
    if (!operations_.nodes || !operations_.publish || !operations_.linked_targets
        || !operations_.target || !operations_.copy || !operations_.prepare || !operations_.write
        || !editor_.capture || !editor_.current || !editor_.reserve_publication
        || !editor_.reserve_target || !editor_.accept || !editor_.release)
        throw std::invalid_argument("all shared Grade Node lifecycle operations are required");
    connect(
        &publication_watcher_,
        &QFutureWatcher<PublicationResult>::finished,
        this,
        &SharedGradeLinkController::finishPublication
    );
    connect(
        &setup_watcher_,
        &QFutureWatcher<SetupResult>::finished,
        this,
        &SharedGradeLinkController::finishSetup
    );
    connect(
        &prepare_watcher_,
        &QFutureWatcher<PrepareResult>::finished,
        this,
        &SharedGradeLinkController::finishPrepare
    );
    connect(
        &write_watcher_,
        &QFutureWatcher<WriteResult>::finished,
        this,
        &SharedGradeLinkController::finishWrite
    );
    connect(
        &preview_watcher_,
        &QFutureWatcher<PreviewResult>::finished,
        this,
        &SharedGradeLinkController::finishPreview
    );
    if (auto* app = QCoreApplication::instance())
        app->installEventFilter(this);
}

SharedGradeLinkController::~SharedGradeLinkController() {
    if (cancellation_)
        cancellation_->cancel();
    if (preview_cancellation_)
        preview_cancellation_->cancel();
    publication_watcher_.waitForFinished();
    setup_watcher_.waitForFinished();
    prepare_watcher_.waitForFinished();
    write_watcher_.waitForFinished();
    preview_watcher_.waitForFinished();
    if (reserved_)
        editor_.release();
}

bool SharedGradeLinkController::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::LanguageChange)
        emit changed();
    return QObject::eventFilter(watched, event);
}

QString SharedGradeLinkController::nodeName() const {
    const auto& source = request_source_ ? *request_source_ : source_;
    const auto* node = source_node(source);
    if (node && !node->shared_layer_id.isEmpty()) {
        if (!source.shared_name.isEmpty())
            return source.shared_name;
        if (node->shared_layer_id == published_.layer_id)
            return published_.label;
    }
    return node ? node->label : published_.label;
}
int SharedGradeLinkController::completed() const {
    return completed_;
}
int SharedGradeLinkController::failed() const {
    return failed_;
}
bool SharedGradeLinkController::canRetry() const {
    return !busy_ && admissionEnabled()
           && (publication_failed_ || setup_failed_ || !retry_rows_.isEmpty()
               || subscriptions_pending_ || selected_cursor_ < selected_targets_.size());
}
QString SharedGradeLinkController::statusText() const {
    if (!error_.isEmpty())
        return error_.translated();
    if (publishing_)
        return tr("Publishing shared node…");
    if (setup_running_)
        return tr("Loading linked photos…");
    if (previewing_)
        return tr("Updating preview · %1 completed").arg(completed());
    if (busy_)
        return tr("Updating shared links · %1 completed").arg(completed());
    if (subscriptions_pending_ || selected_cursor_ < selected_targets_.size())
        return tr("%1 completed · %2 need attention · more links pending")
            .arg(completed())
            .arg(failed());
    if (total() > 0)
        return tr("%1 completed · %2 need attention").arg(completed()).arg(failed());
    if (!published_.layer_id.isEmpty())
        return tr("Shared node published · V%1").arg(publishedRevisionNumber());
    return {};
}
QString SharedGradeLinkController::issueText(BackendSharedGradeLinkStatus issue) const {
    return issue_message(issue).translated();
}
QVariantList SharedGradeLinkController::results() const {
    QVariantList values;
    auto append = [&](const Row& row) {
        QString message = issueText(row.issue);
        if (row.state == QStringLiteral("running"))
            message = tr("Updating…");
        else if (row.state == QStringLiteral("cancelled"))
            message = tr("Stopped · previous edits kept");
        else if (row.state == QStringLiteral("published"))
            message = tr("Published and linked");
        // These typed outcomes already have a complete localized explanation.
        // Keep the engine receipt in the row without repeating its raw text.
        const bool explained = row.issue == BackendSharedGradeLinkStatus::LocalChanged
                               || row.issue == BackendSharedGradeLinkStatus::HeadChanged
                               || row.issue == BackendSharedGradeLinkStatus::NodeLimit;
        const QString diagnostic = explained ? QString{} : row.detail;
        values.push_back(
            QVariantMap{
                {"photoId", row.target.photo.photo_id},
                {"sourcePath", row.target.photo.source_path},
                {"title", row.target.title},
                {"layerId", row.shared ? row.shared->layer_id : QString{}},
                {"state", row.state},
                {"message", message},
                {"detail",
                 row.message.isEmpty()
                     ? diagnostic
                     : row.message.translated()
                           + (diagnostic.isEmpty() ? QString{} : QLatin1Char('\n') + diagnostic)},
            }
        );
    };
    for (const auto& row : result_rows_)
        append(row);
    if (active_row_ >= 0)
        append(rows_.at(active_row_));
    return values;
}

bool SharedGradeLinkController::captureRequest(const QString& mode) {
    if (busy_ || !admissionEnabled())
        return false;
    try {
        auto captured = editor_.capture();
        const auto* node = source_node(captured);
        if (!node)
            throw std::runtime_error(tr("Select an adjustment node first.").toStdString());
        if (mode == QStringLiteral("link")
            && (node->shared_layer_id.isEmpty() || node->shared_revision_id.isEmpty()))
            throw std::runtime_error(
                tr("Publish this node before linking it to other photos.").toStdString()
            );
        request_source_ = std::move(captured);
        error_.clear();
        emit changed();
        return true;
    } catch (const std::exception& error) {
        error_ = {
            "SharedGradeLinkController",
            QT_TRANSLATE_NOOP("SharedGradeLinkController", "Could not prepare sharing · %1"),
            {QString::fromUtf8(error.what())}
        };
        emit changed();
        return false;
    }
}
void SharedGradeLinkController::requestPublication() {
    if (captureRequest(QStringLiteral("publish")))
        emit presentationRequested(QStringLiteral("publish"));
}
void SharedGradeLinkController::requestLinks() {
    if (captureRequest(QStringLiteral("link")))
        emit presentationRequested(QStringLiteral("link"));
}
bool SharedGradeLinkController::refreshFrozenSourceHead(SharedGradeLinkSource& source) {
    try {
        const auto current = editor_.capture();
        if (current.state.photo_id != source.state.photo_id
            || current.state.source_path != source.state.source_path
            || current.representation_id != source.representation_id
            || current.state.active_variant_id != source.state.active_variant_id
            || current.grade_node_id != source.grade_node_id
            || current.state.is_version_draft != source.state.is_version_draft
            || current.state.grade_stack != source.state.grade_stack)
            return false;
        // Only an editor-acknowledged head advance with the exact same frozen
        // content may be adopted. External Catalog heads are never rebased here.
        source.state.base_commit_id = current.state.base_commit_id;
        source.expected_working_commit_id = current.expected_working_commit_id;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}
bool SharedGradeLinkController::normalizeTargets(
    const QVariantList& targets,
    const QString& source_photo_id,
    QVector<SharedGradeLinkTarget>& normalized
) {
    QSet<QString> seen;
    for (const auto& value : targets) {
        const auto map = value.toMap();
        const auto id = map.value("photoId").toString();
        const auto path = map.value("sourcePath").toString();
        if (id.isEmpty() || id == source_photo_id || seen.contains(id))
            continue;
        if (normalized.size() >= retry_limit) {
            error_ = {
                "SharedGradeLinkController",
                QT_TRANSLATE_NOOP(
                    "SharedGradeLinkController",
                    "Select at most 1,000 photos per linking operation."
                )
            };
            emit changed();
            return false;
        }
        seen.insert(id);
        auto title = map.value("title").toString();
        if (title.isEmpty())
            title = QFileInfo(path).fileName();
        normalized.push_back({{id, path}, title, std::nullopt});
    }
    return true;
}
void SharedGradeLinkController::publish(
    const QString& label,
    const QVariantList& targets,
    bool allow_spatial_masks
) {
    if (busy_ || !admissionEnabled())
        return;
    if (label.trimmed().isEmpty()) {
        error_ = {
            "SharedGradeLinkController",
            QT_TRANSLATE_NOOP("SharedGradeLinkController", "Give the shared node a name.")
        };
        emit changed();
        return;
    }
    if (!request_source_ && !captureRequest(QStringLiteral("publish")))
        return;
    auto candidate = *request_source_;
    if (!refreshFrozenSourceHead(candidate)) {
        error_ = {
            "SharedGradeLinkController",
            QT_TRANSLATE_NOOP(
                "SharedGradeLinkController",
                "This source photo changed. Open sharing again to use its current draft."
            )
        };
        emit changed();
        return;
    }
    QVector<SharedGradeLinkTarget> normalized;
    if (!normalizeTargets(targets, candidate.state.photo_id, normalized))
        return;
    source_ = std::move(candidate);
    selected_targets_ = std::move(normalized);
    request_source_.reset();
    published_ = {};
    clipboard_ = {};
    rows_.clear();
    result_rows_.clear();
    retry_rows_.clear();
    queue_.clear();
    selected_subscription_ids_.clear();
    subscription_cursor_.clear();
    selected_cursor_ = completed_ = failed_ = 0;
    total_ = 0;
    publication_label_ = label.trimmed();
    allow_spatial_masks_ = allow_spatial_masks;
    update_subscriptions_ = subscriptions_pending_ = true;
    refresh_current_ = false;
    setup_initialized_ = setup_failed_ = publication_failed_ = false;
    error_.clear();
    cancellation_ = std::make_shared<AdjustmentTransferCancellation>();
    busy_ = publishing_ = true;
    reservation_attempts_ = 0;
    emit changed();
    startPublication();
}
void SharedGradeLinkController::linkToPhotos(
    const QVariantList& targets,
    bool allow_spatial_masks
) {
    if (busy_ || !admissionEnabled())
        return;
    if (!request_source_ && !captureRequest(QStringLiteral("link")))
        return;
    auto candidate = *request_source_;
    if (!refreshFrozenSourceHead(candidate)) {
        error_ = {
            "SharedGradeLinkController",
            QT_TRANSLATE_NOOP(
                "SharedGradeLinkController",
                "This source photo changed. Open sharing again to use its current draft."
            )
        };
        emit changed();
        return;
    }
    QVector<SharedGradeLinkTarget> normalized;
    if (!normalizeTargets(targets, candidate.state.photo_id, normalized))
        return;
    if (normalized.isEmpty()) {
        error_ = {
            "SharedGradeLinkController",
            QT_TRANSLATE_NOOP("SharedGradeLinkController", "Select other photos to link.")
        };
        emit changed();
        return;
    }
    source_ = std::move(candidate);
    selected_targets_ = std::move(normalized);
    request_source_.reset();
    published_ = {};
    clipboard_ = {};
    rows_.clear();
    result_rows_.clear();
    retry_rows_.clear();
    queue_.clear();
    selected_subscription_ids_.clear();
    subscription_cursor_.clear();
    selected_cursor_ = total_ = completed_ = failed_ = 0;
    allow_spatial_masks_ = allow_spatial_masks;
    update_subscriptions_ = subscriptions_pending_ = refresh_current_ = false;
    setup_initialized_ = setup_failed_ = publication_failed_ = false;
    error_.clear();
    cancellation_ = std::make_shared<AdjustmentTransferCancellation>();
    busy_ = true;
    startSetup();
}
void SharedGradeLinkController::syncSelected() {
    startRefresh(true);
}
void SharedGradeLinkController::refreshCurrentLinks() {
    startRefresh(false);
}
void SharedGradeLinkController::startRefresh(bool selected_only) {
    if (busy_ || !admissionEnabled())
        return;
    if (!captureRequest(QStringLiteral("refresh")))
        return;
    source_ = *request_source_;
    request_source_.reset();
    published_ = {};
    clipboard_ = {};
    selected_targets_.clear();
    rows_.clear();
    result_rows_.clear();
    retry_rows_.clear();
    queue_.clear();
    selected_cursor_ = total_ = completed_ = failed_ = 0;
    update_subscriptions_ = subscriptions_pending_ = false;
    refresh_current_ = true;
    refresh_selected_only_ = selected_only;
    setup_initialized_ = setup_failed_ = publication_failed_ = false;
    error_.clear();
    cancellation_ = std::make_shared<AdjustmentTransferCancellation>();
    busy_ = true;
    startSetup();
}

void SharedGradeLinkController::startPublication() {
    if (cancellation_->cancelled()) {
        publication_failed_ = true;
        error_ = {
            "SharedGradeLinkController",
            QT_TRANSLATE_NOOP(
                "SharedGradeLinkController",
                "Publication stopped · previous edits kept"
            )
        };
        finishQueue();
        return;
    }
    auto admission = editor_.reserve_publication(source_);
    if (admission == AdjustmentTransferAdmission::Changed && refreshFrozenSourceHead(source_))
        admission = editor_.reserve_publication(source_);
    if (admission == AdjustmentTransferAdmission::Busy && ++reservation_attempts_ < 100) {
        QTimer::singleShot(50, this, &SharedGradeLinkController::startPublication);
        return;
    }
    if (admission != AdjustmentTransferAdmission::Ready) {
        publication_failed_ = admission == AdjustmentTransferAdmission::Busy;
        if (publication_failed_) {
            error_ = {
                "SharedGradeLinkController",
                QT_TRANSLATE_NOOP(
                    "SharedGradeLinkController",
                    "The editor is busy. Retry after the current operation finishes."
                )
            };
            finishQueue();
            return;
        }
        subscriptions_pending_ = false;
        selected_cursor_ = static_cast<int>(selected_targets_.size());
        error_ = {
            "SharedGradeLinkController",
            QT_TRANSLATE_NOOP(
                "SharedGradeLinkController",
                "This source photo changed. Open sharing again to publish its current draft."
            )
        };
        finishQueue();
        return;
    }
    reserved_ = true;
    publication_watcher_.setFuture(
        QtConcurrent::run([operation = operations_.publish,
                           source = source_,
                           label = publication_label_,
                           cancellation = cancellation_] {
            PublicationResult result;
            if (cancellation->cancelled()) {
                result.cancelled = true;
                return result;
            }
            try {
                result.receipt = operation(source, label);
            } catch (const std::exception& error) {
                result.error = QString::fromUtf8(error.what());
            }
            return result;
        })
    );
}
void SharedGradeLinkController::finishPublication() {
    const auto result = publication_watcher_.future().takeResult();
    publishing_ = false;
    if (result.error.isEmpty() && !result.cancelled) {
        published_ = result.receipt.shared_node;
        editor_.accept(
            result.receipt.state,
            source_.state,
            source_.grade_node_id,
            QStringLiteral("grade_node/%1/shared_publish").arg(source_.grade_node_id)
        );
        source_.state = result.receipt.state;
        source_.expected_working_commit_id = source_.state.base_commit_id;
        Row source_row;
        source_row.target =
            {{source_.state.photo_id, source_.state.source_path}, source_.name, std::nullopt};
        source_row.shared = std::make_shared<const BackendSharedGradeNode>(published_);
        source_row.state = QStringLiteral("published");
        source_row.issue = BackendSharedGradeLinkStatus::Updated;
        source_row.source_publication = true;
        recordResult(source_row);
        emit sharedNodesChanged();
        emit photosChanged();
    } else {
        publication_failed_ = true;
        error_ =
            result.cancelled
                ? LocalizedUiMessage{"SharedGradeLinkController", QT_TRANSLATE_NOOP("SharedGradeLinkController", "Publication stopped · previous edits kept")}
                : LocalizedUiMessage{
                      "SharedGradeLinkController",
                      QT_TRANSLATE_NOOP(
                          "SharedGradeLinkController",
                          "Could not publish shared node · %1"
                      ),
                      {result.error}
                  };
    }
    editor_.release();
    reserved_ = false;
    if (publication_failed_ || cancellation_->cancelled())
        finishQueue();
    else
        startSetup();
}

void SharedGradeLinkController::startSetup() {
    setup_running_ = true;
    emit changed();
    setup_watcher_.setFuture(
        QtConcurrent::run([operations = operations_,
                           source = source_,
                           published = published_,
                           selected = selected_targets_,
                           initialized = setup_initialized_,
                           update_subscriptions =
                               update_subscriptions_
                               && (!setup_initialized_ || subscriptions_pending_),
                           refresh = refresh_current_,
                           selected_only = refresh_selected_only_,
                           allow_spatial = allow_spatial_masks_,
                           cursor = subscription_cursor_,
                           cancellation = cancellation_] {
            SetupResult result;
            result.shared = published;
            try {
                if (cancellation->cancelled()) {
                    result.cancelled = true;
                    return result;
                }
                if (refresh && !initialized) {
                    const auto heads = operations.nodes();
                    for (const auto& head : heads) {
                        QVector<QString> expected_nodes, expected_revisions;
                        for (const auto& node : source.state.grade_stack.grade_nodes) {
                            if (!node.shared_follow_head || node.shared_layer_id != head.layer_id
                                || (selected_only && node.grade_node_id != source.grade_node_id))
                                continue;
                            expected_nodes.push_back(node.grade_node_id);
                            expected_revisions.push_back(node.shared_revision_id);
                        }
                        if (!expected_nodes.isEmpty()) {
                            auto target = operations.target(
                                head.layer_id,
                                source.state.photo_id,
                                source.state.source_path
                            );
                            if (target.photo_id != source.state.photo_id
                                || target.source_path != source.state.source_path
                                || target.representation_id != source.representation_id
                                || target.variant_id != source.state.active_variant_id
                                || target.working_commit_id != source.expected_working_commit_id
                                || target.grade_node_ids.size()
                                       != target.shared_revision_ids.size()) {
                                result.issue = BackendSharedGradeLinkStatus::HeadChanged;
                                return result;
                            }
                            for (qsizetype index = 0; index < expected_nodes.size(); ++index) {
                                const auto found =
                                    target.grade_node_ids.indexOf(expected_nodes[index]);
                                if (found < 0
                                    || target.shared_revision_ids[found]
                                           != expected_revisions[index]) {
                                    result.issue = BackendSharedGradeLinkStatus::HeadChanged;
                                    return result;
                                }
                            }
                            // Preserve every backend-supplied source fingerprint
                            // and narrow only the intended instance identities.
                            target.grade_node_ids = std::move(expected_nodes);
                            target.shared_revision_ids = std::move(expected_revisions);
                            Row row;
                            row.target =
                                {{target.photo_id, target.source_path}, source.name, target};
                            row.shared = std::make_shared<const BackendSharedGradeNode>(head);
                            result.rows.push_back(std::move(row));
                        }
                    }
                    return result;
                }
                if (!initialized && result.shared.layer_id.isEmpty()) {
                    const auto* node = source_node(source);
                    if (!node)
                        throw std::runtime_error("captured shared node is no longer available");
                    const auto heads = operations.nodes();
                    const auto found =
                        std::find_if(heads.cbegin(), heads.cend(), [&](const auto& head) {
                            return head.layer_id == node->shared_layer_id;
                        });
                    if (found == heads.cend()) {
                        result.issue = BackendSharedGradeLinkStatus::Unavailable;
                        return result;
                    }
                    if (node->shared_revision_id != found->revision_id) {
                        result.issue = BackendSharedGradeLinkStatus::HeadChanged;
                        return result;
                    }
                    if (sharedGradeParametersDiffer(*node, found->grade_node)) {
                        result.issue = BackendSharedGradeLinkStatus::LocalChanged;
                        return result;
                    }
                    result.shared = *found;
                }
                // Existing links do not need source-mask transfer. Finish their
                // metadata pages before preparing any newly selected links.
                if (!update_subscriptions && !selected.isEmpty()) {
                    const auto* node = source_node(source);
                    if (!node)
                        throw std::runtime_error("captured source node is no longer available");
                    auto stack = source.state.grade_stack;
                    stack.grade_nodes = {*node};
                    result.clipboard = operations.copy(
                        {source.state.photo_id,
                         source.state.source_path,
                         source.state.base_commit_id,
                         source.name,
                         std::move(stack),
                         allow_spatial}
                    );
                }
                if (update_subscriptions) {
                    const auto page = operations.linked_targets(
                        result.shared.layer_id,
                        result.shared.revision_id,
                        cursor,
                        page_size
                    );
                    if (page.targets.size() > page_size)
                        throw std::runtime_error(
                            "shared subscription page exceeded its requested bound"
                        );
                    if (page.has_more
                        && (page.next_photo_id.isEmpty() || page.next_photo_id <= cursor))
                        throw std::runtime_error("shared subscription cursor did not advance");
                    result.has_more = page.has_more;
                    result.next_photo_id = page.next_photo_id;
                    QSet<QString> seen;
                    const auto shared =
                        std::make_shared<const BackendSharedGradeNode>(result.shared);
                    for (const auto& target : page.targets) {
                        if (seen.contains(target.photo_id))
                            continue;
                        if (target.photo_id.isEmpty())
                            throw std::runtime_error(
                                "shared subscription photo identity is incomplete"
                            );
                        seen.insert(target.photo_id);
                        Row row;
                        row.target = {
                            {target.photo_id, target.source_path},
                            QFileInfo(target.source_path).fileName(),
                            target
                        };
                        row.shared = shared;
                        result.rows.push_back(std::move(row));
                    }
                }
            } catch (const std::exception& error) {
                result.error = QString::fromUtf8(error.what());
                result.issue = BackendSharedGradeLinkStatus::Failed;
            }
            result.cancelled = cancellation->cancelled();
            return result;
        })
    );
}
void SharedGradeLinkController::finishSetup() {
    auto result = setup_watcher_.future().takeResult();
    setup_running_ = false;
    if (result.cancelled || !result.error.isEmpty()
        || result.issue != BackendSharedGradeLinkStatus::Ready) {
        setup_failed_ = true;
        if (!result.error.isEmpty())
            error_ = {
                "SharedGradeLinkController",
                QT_TRANSLATE_NOOP(
                    "SharedGradeLinkController",
                    "Could not prepare shared links · %1"
                ),
                {result.error}
            };
        else if (!result.cancelled)
            error_ = issue_message(result.issue);
        finishQueue();
        return;
    }
    setup_failed_ = false;
    if (!setup_initialized_) {
        published_ = result.shared;
    }
    if (!result.clipboard.document.isEmpty()) {
        clipboard_ = std::move(result.clipboard);
    }
    setup_initialized_ = true;
    subscriptions_pending_ = result.has_more;
    subscription_cursor_ = result.next_photo_id;
    rows_ = std::move(result.rows);
    queue_.clear();
    total_ += static_cast<int>(rows_.size());
    for (int index = 0; index < rows_.size(); ++index) {
        queue_.push_back(index);
        const auto& id = rows_[index].target.photo.photo_id;
        if (std::any_of(
                selected_targets_.cbegin(),
                selected_targets_.cend(),
                [&](const auto& target) { return target.photo.photo_id == id; }
            ))
            selected_subscription_ids_.insert(id);
    }
    emit changed();
    QTimer::singleShot(0, this, &SharedGradeLinkController::next);
}
void SharedGradeLinkController::startSelectedPage() {
    rows_.clear();
    queue_.clear();
    const auto shared = std::make_shared<const BackendSharedGradeNode>(published_);
    while (selected_cursor_ < selected_targets_.size() && rows_.size() < page_size) {
        const auto target = selected_targets_.at(selected_cursor_++);
        if (selected_subscription_ids_.contains(target.photo.photo_id))
            continue;
        Row row;
        row.target = target;
        row.shared = shared;
        rows_.push_back(std::move(row));
        queue_.push_back(static_cast<int>(rows_.size()) - 1);
    }
    total_ += static_cast<int>(rows_.size());
    emit changed();
    QTimer::singleShot(0, this, &SharedGradeLinkController::next);
}
void SharedGradeLinkController::next() {
    if (cancellation_->cancelled()) {
        for (const auto index : queue_) {
            auto& row = rows_[index];
            row.state = QStringLiteral("cancelled");
            ++failed_;
            recordResult(row);
            retry_rows_.push_back(row);
        }
        queue_.clear();
        finishQueue();
        return;
    }
    if (queue_.isEmpty()) {
        active_row_ = -1;
        while (selected_cursor_ < selected_targets_.size()
               && selected_subscription_ids_.contains(
                   selected_targets_[selected_cursor_].photo.photo_id
               ))
            ++selected_cursor_;
        if (retry_rows_.size() >= retry_limit
            && (subscriptions_pending_ || selected_cursor_ < selected_targets_.size())) {
            error_ = {
                "SharedGradeLinkController",
                QT_TRANSLATE_NOOP(
                    "SharedGradeLinkController",
                    "Many links need attention. Retry these before continuing."
                )
            };
            finishQueue();
        } else if (subscriptions_pending_)
            startSetup();
        else if (selected_cursor_ < selected_targets_.size()) {
            if (clipboard_.document.isEmpty())
                startSetup();
            else
                startSelectedPage();
        } else
            finishQueue();
        return;
    }
    const auto visible = editor_.current().photo_id;
    const auto preferred = std::find_if(queue_.begin(), queue_.end(), [&](int index) {
        return rows_[index].target.photo.photo_id == visible;
    });
    if (preferred != queue_.end())
        std::iter_swap(queue_.begin(), preferred);
    active_row_ = queue_.takeFirst();
    auto& row = rows_[active_row_];
    row.state = QStringLiteral("running");
    row.target.subject_masks_allowed =
        editor_.subject_masks_allowed && editor_.subject_masks_allowed();
    prepare_watcher_.setFuture(
        QtConcurrent::run([operation = operations_.prepare,
                           shared = row.shared,
                           clipboard = clipboard_,
                           target = row.target,
                           cancellation = cancellation_] {
            PrepareResult result;
            try {
                result.prepared = operation(*shared, clipboard, target, cancellation);
            } catch (const std::exception& error) {
                result.prepared.status = BackendSharedGradeLinkStatus::Failed;
                result.prepared.detail = QString::fromUtf8(error.what());
            }
            result.cancelled = cancellation->cancelled();
            return result;
        })
    );
    emit changed();
}
void SharedGradeLinkController::finishPrepare() {
    auto result = prepare_watcher_.future().takeResult();
    auto& target = rows_[active_row_].target;
    // Retain the first exact target even when preparation is cancelled or its
    // reservation fails. A retry cannot append into another Variant/source.
    if (!target.frozen_before && result.prepared.frozen_before)
        target.frozen_before = result.prepared.frozen_before;
    if (!target.subscription && !result.prepared.subscription.grade_node_ids.isEmpty())
        target.subscription = result.prepared.subscription;
    if (result.cancelled || cancellation_->cancelled())
        finishRow(QStringLiteral("cancelled"), BackendSharedGradeLinkStatus::Ready);
    else if (result.prepared.status == BackendSharedGradeLinkStatus::Unchanged)
        finishRow(QStringLiteral("unchanged"), result.prepared.status, result.prepared.detail);
    else if (result.prepared.status != BackendSharedGradeLinkStatus::Ready)
        finishRow(
            QStringLiteral("failed"),
            result.prepared.status,
            result.prepared.detail,
            result.prepared.message
        );
    else {
        prepared_ = std::move(result.prepared);
        reservation_attempts_ = 0;
        reserveAndWrite();
    }
}
void SharedGradeLinkController::reserveAndWrite() {
    if (cancellation_->cancelled()) {
        finishRow(QStringLiteral("cancelled"), BackendSharedGradeLinkStatus::Ready);
        return;
    }
    if (prepared_.new_link && clipboard_.requires_inference
        && (!editor_.subject_masks_allowed || !editor_.subject_masks_allowed())) {
        finishRow(QStringLiteral("failed"), BackendSharedGradeLinkStatus::Unavailable);
        return;
    }
    const auto admission = editor_.reserve_target(prepared_.before);
    if (admission == AdjustmentTransferAdmission::Busy && ++reservation_attempts_ < 100) {
        QTimer::singleShot(50, this, &SharedGradeLinkController::reserveAndWrite);
        return;
    }
    if (admission != AdjustmentTransferAdmission::Ready) {
        finishRow(
            QStringLiteral("failed"),
            admission == AdjustmentTransferAdmission::Busy
                ? BackendSharedGradeLinkStatus::Failed
                : BackendSharedGradeLinkStatus::HeadChanged,
            {},
            admission == AdjustmentTransferAdmission::Busy
                ? LocalizedUiMessage{"SharedGradeLinkController", QT_TRANSLATE_NOOP("SharedGradeLinkController", "The editor is busy. Retry after the current operation finishes.")}
                : LocalizedUiMessage{}
        );
        return;
    }
    reserved_ = true;
    write_watcher_.setFuture(
        QtConcurrent::run([operation = operations_.write,
                           shared = rows_[active_row_].shared,
                           prepared = prepared_] {
            WriteResult result;
            try {
                result.receipt = operation(*shared, prepared);
            } catch (const std::exception& error) {
                result.error = QString::fromUtf8(error.what());
            }
            return result;
        })
    );
}
void SharedGradeLinkController::finishWrite() {
    auto result = write_watcher_.future().takeResult();
    const bool updated =
        result.error.isEmpty() && result.receipt.status == BackendSharedGradeLinkStatus::Updated;
    if (updated) {
        // A different layer on the same active photo may continue from this
        // operation's acknowledged metadata write, never an unrelated head.
        for (const auto index : queue_) {
            auto& pending = rows_[index];
            if (!pending.target.subscription || !pending.shared)
                continue;
            auto& target = *pending.target.subscription;
            if (target.photo_id != result.receipt.state.photo_id
                || target.source_path != result.receipt.state.source_path
                || target.variant_id != result.receipt.state.active_variant_id
                || target.working_commit_id != prepared_.before.base_commit_id || prepared_.new_link
                || target.representation_id != prepared_.subscription.representation_id
                || target.source_byte_len != prepared_.subscription.source_byte_len
                || target.has_source_modified_at_ms
                       != prepared_.subscription.has_source_modified_at_ms
                || target.source_modified_at_ms != prepared_.subscription.source_modified_at_ms)
                continue;
            const auto& nodes = result.receipt.state.grade_stack.grade_nodes;
            bool unchanged_instances =
                target.grade_node_ids.size() == target.shared_revision_ids.size();
            for (qsizetype slot = 0; unchanged_instances && slot < target.grade_node_ids.size();
                 ++slot) {
                unchanged_instances =
                    std::any_of(nodes.cbegin(), nodes.cend(), [&](const auto& node) {
                        return node.grade_node_id == target.grade_node_ids[slot]
                               && node.shared_layer_id == pending.shared->layer_id
                               && node.shared_follow_head
                               && node.shared_revision_id == target.shared_revision_ids[slot];
                    });
            }
            if (unchanged_instances)
                target.working_commit_id = result.receipt.state.base_commit_id;
        }
        QString selected;
        if (prepared_.new_link && !result.receipt.state.grade_stack.grade_nodes.isEmpty())
            selected = result.receipt.state.grade_stack.grade_nodes.constLast().grade_node_id;
        editor_.accept(
            result.receipt.state,
            prepared_.before,
            selected,
            prepared_.new_link ? QStringLiteral("shared/link") : QStringLiteral("shared/update")
        );
    }
    editor_.release();
    reserved_ = false;
    if (!result.error.isEmpty()) {
        finishRow(QStringLiteral("failed"), BackendSharedGradeLinkStatus::Failed, result.error);
    } else if (!updated) {
        finishRow(
            result.receipt.status == BackendSharedGradeLinkStatus::Unchanged
                ? QStringLiteral("unchanged")
                : QStringLiteral("failed"),
            result.receipt.status,
            result.receipt.detail
        );
    } else {
        rows_[active_row_].state = QStringLiteral("updated");
        // Cancellation after the acknowledged CAS keeps its receipt and state.
        if (operations_.preview && !cancellation_->cancelled()
            && editor_.current().photo_id != result.receipt.state.photo_id) {
            preview_state_ = std::move(result.receipt.state);
            previewing_ = true;
            emit changed();
            startPreview();
        } else {
            emit photosChanged();
            finishRow(QStringLiteral("updated"), BackendSharedGradeLinkStatus::Updated);
        }
    }
}
void SharedGradeLinkController::startPreview() {
    if (cancellation_->cancelled()) {
        emit photosChanged();
        finishRow(QStringLiteral("updated"), BackendSharedGradeLinkStatus::Updated);
        return;
    }
    if (editor_.foreground_busy && editor_.foreground_busy()) {
        QTimer::singleShot(50, this, &SharedGradeLinkController::startPreview);
        return;
    }
    preview_cancellation_ = std::make_shared<AdjustmentTransferCancellation>();
    preview_watcher_.setFuture(
        QtConcurrent::run([operation = operations_.preview,
                           state = preview_state_,
                           cancellation = preview_cancellation_] {
            PreviewResult result;
            try {
                result.completed = operation(state, cancellation);
            } catch (const std::exception& error) {
                result.error = QString::fromUtf8(error.what());
            }
            return result;
        })
    );
}
void SharedGradeLinkController::finishPreview() {
    const auto result = preview_watcher_.future().takeResult();
    if (!cancellation_->cancelled() && result.error.isEmpty() && !result.completed
        && preview_cancellation_->cancelled()) {
        QTimer::singleShot(50, this, &SharedGradeLinkController::startPreview);
        return;
    }
    emit photosChanged();
    finishRow(
        QStringLiteral("updated"),
        BackendSharedGradeLinkStatus::Updated,
        {},
        result.error.isEmpty() ? LocalizedUiMessage{}
                               : LocalizedUiMessage{
                                     "SharedGradeLinkController",
                                     QT_TRANSLATE_NOOP(
                                         "SharedGradeLinkController",
                                         "Edits saved; preview could not be refreshed · %1"
                                     ),
                                     {result.error}
                                 }
    );
}
void SharedGradeLinkController::recordResult(const Row& row) {
    const auto key = result_key(row.target, row.shared.get());
    const auto old = std::find_if(result_rows_.begin(), result_rows_.end(), [&](const auto& item) {
        return item.source_publication == row.source_publication
               && result_key(item.target, item.shared.get()) == key;
    });
    if (old != result_rows_.end())
        *old = row;
    else {
        if (result_rows_.size() >= result_limit)
            result_rows_.removeFirst();
        result_rows_.push_back(row);
    }
}
void SharedGradeLinkController::finishRow(
    const QString& state,
    BackendSharedGradeLinkStatus issue,
    QString detail,
    LocalizedUiMessage message
) {
    auto& row = rows_[active_row_];
    row.state = state;
    row.issue = issue;
    row.detail = std::move(detail);
    row.message = std::move(message);
    recordResult(row);
    if (state == QStringLiteral("failed") || state == QStringLiteral("cancelled")) {
        ++failed_;
        retry_rows_.push_back(row);
    } else
        ++completed_;
    active_row_ = -1;
    prepared_ = {};
    preview_state_ = {};
    preview_cancellation_.reset();
    previewing_ = false;
    emit changed();
    QTimer::singleShot(0, this, &SharedGradeLinkController::next);
}
void SharedGradeLinkController::finishQueue() {
    active_row_ = -1;
    publishing_ = setup_running_ = previewing_ = busy_ = false;
    emit changed();
}
void SharedGradeLinkController::retryFailed() {
    if (!canRetry())
        return;
    error_.clear();
    cancellation_ = std::make_shared<AdjustmentTransferCancellation>();
    busy_ = true;
    if (publication_failed_) {
        publication_failed_ = false;
        publishing_ = true;
        reservation_attempts_ = 0;
        startPublication();
    } else if (!retry_rows_.isEmpty()) {
        rows_ = std::move(retry_rows_);
        retry_rows_.clear();
        failed_ -= static_cast<int>(rows_.size());
        queue_.clear();
        for (int index = 0; index < rows_.size(); ++index) {
            rows_[index].state = QStringLiteral("pending");
            rows_[index].detail.clear();
            // An explicit retry re-reads only failed target metadata. It still
            // refuses locally changed shared graphs and newer editor drafts.
            rows_[index].target.refresh_subscription = rows_[index].target.subscription.has_value();
            queue_.push_back(index);
        }
        emit changed();
        QTimer::singleShot(0, this, &SharedGradeLinkController::next);
    } else if (setup_failed_ || subscriptions_pending_) {
        startSetup();
    } else if (selected_cursor_ < selected_targets_.size()) {
        startSelectedPage();
    } else
        finishQueue();
}
void SharedGradeLinkController::cancel() {
    if (cancellation_)
        cancellation_->cancel();
    if (preview_cancellation_)
        preview_cancellation_->cancel();
}
void SharedGradeLinkController::setProfileSwitchPending(bool pending) {
    if (profile_switch_pending_ == pending)
        return;
    profile_switch_pending_ = pending;
    if (pending)
        cancel();
    emit changed();
}
void SharedGradeLinkController::foregroundWorkChanged() {
    if (previewing_ && preview_cancellation_ && editor_.foreground_busy
        && editor_.foreground_busy())
        preview_cancellation_->cancel();
}
