#pragma once

#include "edit_types.hpp"

// Bounded metadata receipts. Shared scheduling never carries image payloads.
struct BackendSharedGradePublicationReceipt final {
    BackendSharedGradeNode shared_node;
    BackendPhotoEditState state;
};

struct BackendSharedGradeLinkedTarget final {
    QString photo_id, source_path, representation_id, variant_id, working_commit_id;
    std::uint64_t source_byte_len = 0;
    bool has_source_modified_at_ms = false;
    std::int64_t source_modified_at_ms = 0;
    QVector<QString> grade_node_ids, shared_revision_ids;
};

struct BackendSharedGradeLinkedTargetsPage final {
    QVector<BackendSharedGradeLinkedTarget> targets;
    bool has_more = false;
    QString next_photo_id;
};

enum class BackendSharedGradeLinkStatus {
    Ready,
    Updated,
    Unchanged,
    LocalChanged,
    HeadChanged,
    Unavailable,
    NodeLimit,
    Failed,
};

struct BackendSharedGradeLinkUpdateResult final {
    BackendSharedGradeLinkStatus status = BackendSharedGradeLinkStatus::Failed;
    BackendPhotoEditState state;
    QString detail;
};
