#include "edit_history.hpp"
#include "edit_history_snapshot.hpp"

#include <cstdlib>
#include <iostream>

namespace {
void require(bool value, const char* message) {
    if (!value) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
} // namespace

int main() {
    SessionEditHistory<EditHistorySnapshot> history;
    EditHistorySnapshot before{{}, QStringLiteral("before-ai")};
    auto masked = before;
    BackendGradeNode node;
    node.grade_node_id = QStringLiteral("subject");
    BackendMaskComponent mask;
    mask.component_id = QStringLiteral("managed-component");
    mask.kind = 6;
    node.local_mask_components = {mask};
    masked.stack.grade_nodes.append(node);
    masked.base_commit_id = QStringLiteral("contains-managed-mask");
    history.record("subject/apply", before, masked);

    auto current = masked;
    current.base_commit_id = QStringLiteral("new-autosave-same-pixels");
    auto restored = history.undo(current);
    require(restored && restored->stack == before.stack, "undo restores the pre-AI stack");
    require(restored->base_commit_id == before.base_commit_id, "undo restores its content anchor");
    restored->base_commit_id = QStringLiteral("autosaved-without-mask");
    restored = history.redo(*restored);
    require(restored && restored->stack == masked.stack, "redo restores the managed component");
    require(
        restored->base_commit_id == masked.base_commit_id,
        "redo restores the mask-bearing Recipe"
    );

    history.beginGesture("strength", *restored);
    current = *restored;
    current.base_commit_id = QStringLiteral("another-autosave");
    history.endGesture("strength", current);
    require(history.undoDepth() == 1, "autosave alone does not add an undo step");

    SessionEditHistory<EditHistorySnapshot> raw_history;
    EditHistorySnapshot raw_before{{}, QStringLiteral("before-raw")};
    raw_before.stack.raw_ai_denoise.present = true;
    auto raw_enabled = raw_before;
    raw_enabled.stack.raw_ai_denoise.enabled = true;
    raw_history.record("raw_ai_denoise/enabled", raw_before, raw_enabled);
    auto presented = raw_enabled;
    auto& foundation = presented.stack.foundation;
    foundation.as_shot_white_balance_available = true;
    foundation.as_shot_temperature_kelvin = foundation.temperature_kelvin = 5'972;
    foundation.as_shot_tint = foundation.tint = 52;
    auto raw_undo = raw_history.undo(presented);
    require(
        raw_undo && !raw_undo->stack.raw_ai_denoise.enabled && raw_history.canRedo(),
        "late camera calibration does not clear or consume the real RAW undo"
    );
    auto raw_redo = raw_history.redo(*raw_undo);
    require(raw_redo && raw_redo->stack == raw_enabled.stack, "RAW redo retains its authored edit");

    auto manual = raw_before;
    manual.stack.foundation.raw_white_balance_mode = 1;
    manual.stack.foundation.temperature_kelvin = 6'500;
    raw_history.clear();
    raw_history.record("foundation/temperature", raw_before, manual);
    auto unrelated = manual;
    unrelated.stack.foundation.tint = 17;
    require(
        !raw_history.undo(unrelated) && !raw_history.canUndo(),
        "an unrelated authored manual white point still invalidates stale history"
    );
    return EXIT_SUCCESS;
}
