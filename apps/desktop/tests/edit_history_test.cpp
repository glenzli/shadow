#include "edit_history.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct State final {
    int exposure = 0;
    int contrast = 0;
    bool layer_enabled = true;
    std::vector<int> curve_y{0, 100};

    bool operator==(const State&) const = default;
};

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "edit history contract failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void continuous_gesture_is_one_step() {
    SessionEditHistory<State> history;
    State current;

    history.beginGesture("exposure", current);
    for (const int value : {1, 2, 3, 4}) {
        const State before = current;
        current.exposure = value;
        history.record("exposure", before, current);
    }
    history.endGesture("exposure", current);

    require(history.undoDepth() == 1, "a drag must create one undo step");
    current = *history.undo(current);
    require(current == State{}, "undo must restore the pre-drag snapshot");
    current = *history.redo(current);
    require(current.exposure == 4, "redo must restore the drag endpoint");
}

void switching_parameters_splits_steps() {
    SessionEditHistory<State> history;
    State current;

    history.beginGesture("exposure", current);
    State before = current;
    current.exposure = 2;
    history.record("exposure", before, current);

    history.beginGesture("contrast", current);
    before = current;
    current.contrast = 7;
    history.record("contrast", before, current);
    history.endGesture("contrast", current);

    require(history.undoDepth() == 2, "different parameters must be separate steps");
    current = *history.undo(current);
    require(
        current.exposure == 2 && current.contrast == 0,
        "first undo must only restore contrast"
    );
    current = *history.undo(current);
    require(current == State{}, "second undo must restore exposure");
}

void tone_curve_drag_is_one_atomic_recipe_step() {
    SessionEditHistory<State> history;
    State current;

    history.beginGesture("tone_curve/1", current);
    for (int value = 99; value >= 0; --value) {
        const State before = current;
        current.curve_y[1] = value;
        history.record("tone_curve/1", before, current);
    }
    history.endGesture("tone_curve/1", current);

    require(history.undoDepth() == 1, "a curve drag must create one undo step");
    current = *history.undo(current);
    require(
        current.curve_y == std::vector<int>({0, 100}),
        "curve undo must restore the complete point set"
    );
    current = *history.redo(current);
    require(
        current.curve_y == std::vector<int>({0, 0}),
        "curve redo must restore the final drag point"
    );
}

void a_new_edit_clears_redo() {
    SessionEditHistory<State> history;
    State current;
    State before = current;
    current.exposure = 1;
    history.record("exposure", before, current);
    current = *history.undo(current);
    require(history.canRedo(), "undo must make redo available");

    before = current;
    current.contrast = 3;
    history.record("contrast", before, current);
    require(!history.canRedo(), "a new edit must clear the redo branch");
}

void layer_bypass_is_atomic_and_preserves_adjustments() {
    SessionEditHistory<State> history;
    State current;
    current.exposure = 4;
    current.curve_y = {0, 72};

    const State before = current;
    current.layer_enabled = false;
    history.record("layer_enabled", before, current);

    require(history.undoDepth() == 1, "layer bypass must create one undo step");
    current = *history.undo(current);
    require(current.layer_enabled, "undo must enable the layer again");
    require(
        current.exposure == 4 && current.curve_y == std::vector<int>({0, 72}),
        "bypass undo must preserve the complete adjustment payload"
    );
    current = *history.redo(current);
    require(!current.layer_enabled, "redo must restore the bypass state");
    require(
        current.exposure == 4 && current.curve_y == std::vector<int>({0, 72}),
        "bypass redo must preserve the complete adjustment payload"
    );
}

void a_net_noop_gesture_preserves_redo() {
    SessionEditHistory<State> history;
    State current;
    State before = current;
    current.exposure = 5;
    history.record("exposure", before, current);
    current = *history.undo(current);

    history.beginGesture("contrast", current);
    before = current;
    current.contrast = 2;
    history.record("contrast", before, current);
    before = current;
    current.contrast = 0;
    history.record("contrast", before, current);
    history.endGesture("contrast", current);

    require(history.canRedo(), "a cancelled drag must not destroy the redo branch");
}

void clear_starts_a_new_session() {
    SessionEditHistory<State> history;
    State current;
    State before = current;
    current.exposure = 9;
    history.record("exposure", before, current);
    history.clear();
    require(!history.canUndo() && !history.canRedo(), "clear must drop both stacks");
}

} // namespace

int main() {
    continuous_gesture_is_one_step();
    switching_parameters_splits_steps();
    tone_curve_drag_is_one_atomic_recipe_step();
    a_new_edit_clears_redo();
    layer_bypass_is_atomic_and_preserves_adjustments();
    a_net_noop_gesture_preserves_redo();
    clear_starts_a_new_session();
    return EXIT_SUCCESS;
}
