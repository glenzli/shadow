#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>

struct BackendPeopleGroup final {
    QString group_id;
    QString display_name;
    std::uint32_t member_count = 0;
    /// Logical photo identities used only to reject a contradictory session
    /// merge. No face geometry or embedding crosses the desktop boundary.
    QStringList photo_ids;
    QByteArray thumbnail_jpeg;
    bool manually_merged = false;
};

/// Device-local anonymous-person organization summary. Biometric vectors and
/// face geometry remain inside Rust and never enter the Qt object graph.
struct BackendPeopleAnalysisReport final {
    bool has_data = false;
    std::uint32_t analyzed_photos = 0;
    std::uint32_t detected_faces = 0;
    std::uint32_t embedded_faces = 0;
    std::uint32_t skipped_items = 0;
    std::uint32_t ungrouped_faces = 0;
    bool truncated = false;
    QVector<BackendPeopleGroup> groups;
    bool can_undo_merge = false;
};

struct BackendPeopleAnalysisProgress final {
    std::uint64_t job_token = 0;
    QString phase;
    std::uint32_t analyzed_photos = 0;
    std::uint32_t maximum_photos = 0;
    std::uint32_t detected_faces = 0;
    std::uint32_t compared_faces = 0;
    bool cancellation_requested = false;
    bool terminal = false;
};

struct BackendPeopleAnalysisExecution final {
    bool made_progress = false;
    std::uint64_t job_token = 0;
    bool cancelled = false;
    QString diagnostic;
    BackendPeopleAnalysisReport report;
};
