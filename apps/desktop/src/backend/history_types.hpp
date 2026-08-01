#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>

/// Desktop-owned, bounded History contracts. Persistence and semantic diff
/// calculation remain in Rust; Qt owns only asynchronous presentation state.

enum class BackendHistoryRefKind : std::uint8_t {
    Working,
    Branch,
    NamedVersion,
    Tag,
};

struct BackendHistoryRef final {
    QString name;
    BackendHistoryRefKind kind = BackendHistoryRefKind::Branch;
    QString commit_id;
    qint64 updated_at_ms = 0;

    bool operator==(const BackendHistoryRef&) const = default;
};

struct BackendHistoryCursor final {
    qint64 created_at_ms = 0;
    QString commit_id;

    [[nodiscard]] bool empty() const noexcept {
        return created_at_ms == 0 && commit_id.isEmpty();
    }

    bool operator==(const BackendHistoryCursor&) const = default;
};

struct BackendPhotoHistoryEntry final {
    QString commit_id;
    QString name;
    qint64 created_at_ms = 0;
    QStringList parent_commit_ids;
    QVector<BackendHistoryRef> refs;
    bool is_named = false;
    bool is_working = false;
    bool is_root = false;
    bool recipe_schema_changed = false;
    std::uint32_t grade_nodes_added = 0;
    std::uint32_t grade_nodes_removed = 0;
    std::uint32_t grade_nodes_moved = 0;
    std::uint32_t grade_nodes_modified = 0;
    std::uint32_t render_ops_added = 0;
    std::uint32_t render_ops_removed = 0;
    std::uint32_t render_ops_modified = 0;
    std::uint32_t render_op_parameter_blocks_changed = 0;
    QStringList changed_parameter_keys;
    bool has_other_changes = false;

    bool operator==(const BackendPhotoHistoryEntry&) const = default;
};

struct BackendPhotoHistoryPage final {
    QVector<BackendPhotoHistoryEntry> entries;
    bool has_more = false;
    BackendHistoryCursor next_cursor;
};

struct BackendLibraryHistoryEntry final {
    QString commit_id;
    QString message;
    qint64 created_at_ms = 0;
    QStringList parent_commit_ids;
    QVector<BackendHistoryRef> refs;
    bool is_root = false;
    bool is_head = false;
    std::uint32_t photo_changes = 0;
    std::uint32_t shared_grade_changes = 0;
    std::uint32_t mask_changes = 0;
    std::uint32_t style_changes = 0;
    std::uint32_t output_state_changes = 0;

    bool operator==(const BackendLibraryHistoryEntry&) const = default;
};

struct BackendLibraryHistoryPage final {
    QVector<BackendLibraryHistoryEntry> entries;
    bool has_more = false;
    BackendHistoryCursor next_cursor;
};

struct BackendLibraryHistoryRefPage final {
    QVector<BackendHistoryRef> refs;
    bool has_more = false;
    QString next_cursor;
};
