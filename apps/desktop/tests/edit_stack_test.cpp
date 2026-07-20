#include "edit_history.hpp"
#include "edit_stack.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "edit stack contract failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

[[nodiscard]] BackendBasicEditLayer layer(const QString& stem) {
    return {
        .layer_id = stem + QStringLiteral("-layer"),
        .label = stem,
        .exposure_node_id = stem + QStringLiteral("-exposure"),
        .contrast_node_id = stem + QStringLiteral("-contrast"),
        .tone_curve_node_id = stem + QStringLiteral("-curve"),
        .channel_gain_node_id = stem + QStringLiteral("-gain"),
        .saturation_node_id = stem + QStringLiteral("-saturation"),
    };
}

void insertion_and_duplicate_identity_are_lossless() {
    BackendEditSettings settings{.layers = {layer(QStringLiteral("first"))}};
    BackendBasicEditLayer duplicate = layer(QStringLiteral("copy"));
    duplicate.basic.exposure_stops = 1.25;
    duplicate.has_tone_curve = true;
    duplicate.tone_curve_points = {{0.0, 0.0}, {0.5, 0.7}, {1.0, 1.0}};

    int selected = 0;
    require(
        EditStack::insertAfterSelection(settings, duplicate, selected),
        "a fresh layer must insert after the current selection"
    );
    require(selected == 1 && settings.layers.at(1) == duplicate,
            "insert must preserve every layer and node identity plus its payload");
    require(
        settings.layers.at(0).tone_curve_node_id
            != settings.layers.at(1).tone_curve_node_id,
        "a duplicate must reserve a fresh Tone Curve identity even before use"
    );
    require(
        !EditStack::insertAfterSelection(settings, duplicate, selected),
        "a duplicate persistent layer identity must be rejected"
    );
}

void deletion_keeps_one_layer_and_selects_the_nearest_survivor() {
    BackendEditSettings settings{
        .layers = {
            layer(QStringLiteral("first")),
            layer(QStringLiteral("second")),
            layer(QStringLiteral("third")),
        },
    };
    int selected = 2;
    require(EditStack::deleteSelection(settings, selected), "selected layer must delete");
    require(
        selected == 1 && settings.layers.at(1).layer_id == QStringLiteral("second-layer"),
        "deleting the tail must select its nearest surviving neighbor"
    );
    require(EditStack::deleteSelection(settings, selected), "a second layer may delete");
    require(
        !EditStack::deleteSelection(settings, selected),
        "the final executable layer must not be deleted"
    );
}

void reorder_and_selection_resolution_follow_stable_ids() {
    BackendEditSettings settings{
        .layers = {
            layer(QStringLiteral("first")),
            layer(QStringLiteral("second")),
            layer(QStringLiteral("third")),
        },
    };
    int selected = 1;
    require(EditStack::moveSelection(settings, selected, 0), "selected layer must move");
    require(
        selected == 0 && settings.layers.at(0).layer_id == QStringLiteral("second-layer"),
        "moving changes order without changing the selected identity"
    );
    require(
        EditStack::resolvedSelection(settings, QStringLiteral("third-layer"), 0) == 2,
        "selection restoration must prefer the stable layer identity"
    );
}

void insertion_is_bounded_to_sixteen_layers() {
    BackendEditSettings settings{.layers = {layer(QStringLiteral("layer-1"))}};
    int selected = 0;
    for (int index = 2; index <= EditStack::maximum_layer_count; ++index) {
        require(
            EditStack::insertAfterSelection(
                settings,
                layer(QStringLiteral("layer-%1").arg(index)),
                selected
            ),
            "every layer through the desktop bound must insert"
        );
    }
    require(
        !EditStack::insertAfterSelection(
            settings,
            layer(QStringLiteral("overflow")),
            selected
        ),
        "the seventeenth layer must be rejected before crossing the bridge"
    );
}

void full_stack_undo_restores_every_persistent_identity() {
    BackendEditSettings before{
        .layers = {
            layer(QStringLiteral("first")),
            layer(QStringLiteral("second")),
        },
    };
    before.layers[1].has_tone_curve = true;
    before.layers[1].tone_curve_points = {{0.0, 0.1}, {1.0, 0.9}};
    BackendEditSettings after = before;
    int selected = 1;
    require(EditStack::moveSelection(after, selected, 0), "test move must succeed");
    after.layers[0].enabled = false;

    SessionEditHistory<BackendEditSettings> history;
    history.record("layer/second-layer/move", before, after);
    std::string restored_key;
    const auto restored = history.undo(after, &restored_key);
    require(restored && *restored == before, "undo must restore the exact complete stack");
    require(
        restored_key == "layer/second-layer/move",
        "undo must return the stable operation subject for UI reselection"
    );
    const auto redone = history.redo(*restored, &restored_key);
    require(redone && *redone == after, "redo must restore order, bypass, curves, and IDs");
}

} // namespace

int main() {
    insertion_and_duplicate_identity_are_lossless();
    deletion_keeps_one_layer_and_selects_the_nearest_survivor();
    reorder_and_selection_resolution_follow_stable_ids();
    insertion_is_bounded_to_sixteen_layers();
    full_stack_undo_restores_every_persistent_identity();
    return EXIT_SUCCESS;
}
