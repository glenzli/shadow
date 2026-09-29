#pragma once

#include <QtGlobal>

// Opt-in, bounded checkpoints around editor phase transitions. These contain
// generations and byte counts only: never a photo identity, path, or payload.
void log_edit_performance_checkpoint(
    const char* stage,
    quint64 photo_generation,
    quint64 recipe_revision,
    quint64 retained_source_bytes = 0
);
