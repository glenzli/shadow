#pragma once

#include "backend/edit_types.hpp"

// Opaque managed masks can only be recovered from an immutable Recipe that
// contains them. Keep that content anchor with each undo/redo snapshot; the
// current durable working head remains a separate autosave CAS expectation.
struct EditHistorySnapshot final {
    BackendGradeStack stack;
    QString base_commit_id;

    bool operator==(const EditHistorySnapshot& other) const {
        // Autosave may advance the anchor without changing the visible edit.
        return stack == other.stack;
    }
};
