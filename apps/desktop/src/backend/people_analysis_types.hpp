#pragma once

#include <QString>
#include <QVector>

#include <cstdint>

struct BackendPeopleGroup final {
    QString group_id;
    std::uint32_t member_count = 0;
};

/// Session-only anonymous-person analysis summary. Biometric vectors and face
/// geometry remain inside Rust and never enter the Qt object graph.
struct BackendPeopleAnalysisReport final {
    std::uint32_t analyzed_photos = 0;
    std::uint32_t detected_faces = 0;
    std::uint32_t embedded_faces = 0;
    std::uint32_t skipped_items = 0;
    std::uint32_t ungrouped_faces = 0;
    bool truncated = false;
    QVector<BackendPeopleGroup> groups;
};
