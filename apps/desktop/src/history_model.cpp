#include "history_model.hpp"

#include <QVariantList>
#include <QVariantMap>

#include <algorithm>
#include <limits>

namespace {

[[nodiscard]] QString ref_kind_name(const BackendHistoryRefKind kind) {
    switch (kind) {
    case BackendHistoryRefKind::Working:
        return QStringLiteral("working");
    case BackendHistoryRefKind::Branch:
        return QStringLiteral("branch");
    case BackendHistoryRefKind::NamedVersion:
        return QStringLiteral("namedVersion");
    case BackendHistoryRefKind::Tag:
        return QStringLiteral("tag");
    }
    return {};
}

[[nodiscard]] QVariantList refs(const QVector<BackendHistoryRef>& source) {
    QVariantList result;
    result.reserve(source.size());
    for (const auto& reference : source) {
        result.push_back(
            QVariantMap{
                {QStringLiteral("name"), reference.name},
                {QStringLiteral("kind"), ref_kind_name(reference.kind)},
                {QStringLiteral("commitId"), reference.commit_id},
                {QStringLiteral("updatedAtMs"), reference.updated_at_ms},
            }
        );
    }
    return result;
}

template <typename Entry>
[[nodiscard]] bool has_commit(const QVector<Entry>& entries, const QString& commit_id) {
    return std::any_of(entries.cbegin(), entries.cend(), [&commit_id](const Entry& entry) {
        return entry.commit_id == commit_id;
    });
}

[[nodiscard]] int bounded_total(const BackendLibraryHistoryEntry& entry) {
    const std::uint64_t total = static_cast<std::uint64_t>(entry.photo_changes)
                                + static_cast<std::uint64_t>(entry.shared_grade_changes)
                                + static_cast<std::uint64_t>(entry.mask_changes)
                                + static_cast<std::uint64_t>(entry.style_changes)
                                + static_cast<std::uint64_t>(entry.output_state_changes);
    return static_cast<int>(
        std::min<std::uint64_t>(total, static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
    );
}

} // namespace

PhotoHistoryModel::PhotoHistoryModel(QObject* parent) : QAbstractListModel(parent) {}

int PhotoHistoryModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(entries_.size());
}

QVariant PhotoHistoryModel::data(const QModelIndex& index, const int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= entries_.size()) {
        return {};
    }
    const auto& entry = entries_.at(index.row());
    switch (role) {
    case CommitIdRole:
        return entry.commit_id;
    case NameRole:
        return entry.name;
    case CreatedAtMsRole:
        return entry.created_at_ms;
    case ParentCommitIdsRole:
        return entry.parent_commit_ids;
    case RefsRole:
        return refs(entry.refs);
    case NamedRole:
        return entry.is_named;
    case WorkingRole:
        return entry.is_working;
    case RootRole:
        return entry.is_root;
    case RecipeSchemaChangedRole:
        return entry.recipe_schema_changed;
    case GradeNodesAddedRole:
        return entry.grade_nodes_added;
    case GradeNodesRemovedRole:
        return entry.grade_nodes_removed;
    case GradeNodesMovedRole:
        return entry.grade_nodes_moved;
    case GradeNodesModifiedRole:
        return entry.grade_nodes_modified;
    case RenderOpsAddedRole:
        return entry.render_ops_added;
    case RenderOpsRemovedRole:
        return entry.render_ops_removed;
    case RenderOpsModifiedRole:
        return entry.render_ops_modified;
    case ParameterBlocksChangedRole:
        return entry.render_op_parameter_blocks_changed;
    case ChangedParameterKeysRole:
        return entry.changed_parameter_keys;
    case HasOtherChangesRole:
        return entry.has_other_changes;
    default:
        return {};
    }
}

QHash<int, QByteArray> PhotoHistoryModel::roleNames() const {
    return {
        {CommitIdRole, "commitId"},
        {NameRole, "name"},
        {CreatedAtMsRole, "createdAtMs"},
        {ParentCommitIdsRole, "parentCommitIds"},
        {RefsRole, "refs"},
        {NamedRole, "isNamed"},
        {WorkingRole, "isWorking"},
        {RootRole, "isRoot"},
        {RecipeSchemaChangedRole, "recipeSchemaChanged"},
        {GradeNodesAddedRole, "gradeNodesAdded"},
        {GradeNodesRemovedRole, "gradeNodesRemoved"},
        {GradeNodesMovedRole, "gradeNodesMoved"},
        {GradeNodesModifiedRole, "gradeNodesModified"},
        {RenderOpsAddedRole, "renderOpsAdded"},
        {RenderOpsRemovedRole, "renderOpsRemoved"},
        {RenderOpsModifiedRole, "renderOpsModified"},
        {ParameterBlocksChangedRole, "parameterBlocksChanged"},
        {ChangedParameterKeysRole, "changedParameterKeys"},
        {HasOtherChangesRole, "hasOtherChanges"},
    };
}

void PhotoHistoryModel::replace(QVector<BackendPhotoHistoryEntry> entries) {
    beginResetModel();
    entries_ = std::move(entries);
    endResetModel();
}

void PhotoHistoryModel::append(QVector<BackendPhotoHistoryEntry> entries) {
    entries.erase(
        std::remove_if(
            entries.begin(),
            entries.end(),
            [this](const BackendPhotoHistoryEntry& entry) {
                return has_commit(entries_, entry.commit_id);
            }
        ),
        entries.end()
    );
    if (entries.isEmpty()) {
        return;
    }
    const int first = static_cast<int>(entries_.size());
    const int last = first + static_cast<int>(entries.size()) - 1;
    beginInsertRows({}, first, last);
    entries_.append(std::move(entries));
    endInsertRows();
}

void PhotoHistoryModel::clear() {
    if (!entries_.isEmpty()) {
        replace({});
    }
}

LibraryHistoryModel::LibraryHistoryModel(QObject* parent) : QAbstractListModel(parent) {}

int LibraryHistoryModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(entries_.size());
}

QVariant LibraryHistoryModel::data(const QModelIndex& index, const int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= entries_.size()) {
        return {};
    }
    const auto& entry = entries_.at(index.row());
    switch (role) {
    case CommitIdRole:
        return entry.commit_id;
    case MessageRole:
        return entry.message;
    case CreatedAtMsRole:
        return entry.created_at_ms;
    case ParentCommitIdsRole:
        return entry.parent_commit_ids;
    case RefsRole:
        return refs(entry.refs);
    case RootRole:
        return entry.is_root;
    case HeadRole:
        return entry.is_head;
    case PhotoChangesRole:
        return entry.photo_changes;
    case SharedGradeChangesRole:
        return entry.shared_grade_changes;
    case MaskChangesRole:
        return entry.mask_changes;
    case StyleChangesRole:
        return entry.style_changes;
    case OutputStateChangesRole:
        return entry.output_state_changes;
    case TotalChangesRole:
        return bounded_total(entry);
    default:
        return {};
    }
}

QHash<int, QByteArray> LibraryHistoryModel::roleNames() const {
    return {
        {CommitIdRole, "commitId"},
        {MessageRole, "message"},
        {CreatedAtMsRole, "createdAtMs"},
        {ParentCommitIdsRole, "parentCommitIds"},
        {RefsRole, "refs"},
        {RootRole, "isRoot"},
        {HeadRole, "isHead"},
        {PhotoChangesRole, "photoChanges"},
        {SharedGradeChangesRole, "sharedGradeChanges"},
        {MaskChangesRole, "maskChanges"},
        {StyleChangesRole, "styleChanges"},
        {OutputStateChangesRole, "outputStateChanges"},
        {TotalChangesRole, "totalChanges"},
    };
}

void LibraryHistoryModel::replace(QVector<BackendLibraryHistoryEntry> entries) {
    beginResetModel();
    entries_ = std::move(entries);
    endResetModel();
}

void LibraryHistoryModel::append(QVector<BackendLibraryHistoryEntry> entries) {
    entries.erase(
        std::remove_if(
            entries.begin(),
            entries.end(),
            [this](const BackendLibraryHistoryEntry& entry) {
                return has_commit(entries_, entry.commit_id);
            }
        ),
        entries.end()
    );
    if (entries.isEmpty()) {
        return;
    }
    const int first = static_cast<int>(entries_.size());
    const int last = first + static_cast<int>(entries.size()) - 1;
    beginInsertRows({}, first, last);
    entries_.append(std::move(entries));
    endInsertRows();
}

void LibraryHistoryModel::clear() {
    if (!entries_.isEmpty()) {
        replace({});
    }
}
