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
    return EXIT_SUCCESS;
}
