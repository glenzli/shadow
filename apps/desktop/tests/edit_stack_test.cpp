#include "edit_history.hpp"
#include "edit_stack.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "Grade Node stack contract failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

[[nodiscard]] BackendGradeNode grade_node(const QString& stem) {
    return {
        .grade_node_id = stem + QStringLiteral("-grade-node"),
        .label = stem,
        .exposure_render_op_id = stem + QStringLiteral("-exposure"),
        .contrast_render_op_id = stem + QStringLiteral("-contrast"),
        .selective_tone_render_op_id = stem + QStringLiteral("-selective-tone"),
        .tone_curve_render_op_id = stem + QStringLiteral("-curve"),
        .white_balance_render_op_id = stem + QStringLiteral("-white-balance"),
        .saturation_render_op_id = stem + QStringLiteral("-saturation"),
        .perceptual_color_render_op_id = stem + QStringLiteral("-perceptual-color"),
        .sharpen_render_op_id = stem + QStringLiteral("-sharpen"),
    };
}

void insertion_and_duplicate_identity_are_lossless() {
    BackendGradeStack grade_stack{.grade_nodes = {grade_node(QStringLiteral("first"))}};
    BackendGradeNode duplicate = grade_node(QStringLiteral("copy"));
    duplicate.basic.exposure_stops = 1.25;
    duplicate.fine.highlights = -0.35;
    duplicate.fine.vibrance = 0.42;
    duplicate.fine.mixer_saturation[5] = 0.28;
    duplicate.fine.color_range_enabled = true;
    duplicate.fine.color_range_center = 218.0;
    duplicate.fine.sharpen_amount = 0.8;
    duplicate.fine.sharpen_radius = 1.4;
    duplicate.fine.sharpen_threshold = 0.07;
    duplicate.fine.sharpen_masking = 0.35;
    duplicate.tone_curve_kind = ToneCurveKind::SmoothRgb;
    duplicate.tone_curve_master_points = {
        {0.0, 0.0}, {0.5, 0.7}, {1.0, 1.0}
    };
    duplicate.tone_curve_red_points = {{0.0, 0.0}, {1.0, 0.9}};
    duplicate.tone_curve_green_points = {{0.0, 0.0}, {1.0, 1.0}};
    duplicate.tone_curve_blue_points = {{0.0, 0.0}, {1.0, 1.0}};

    int selected = 0;
    require(
        GradeNodeStack::insertAfterSelection(grade_stack, duplicate, selected),
        "a fresh Grade Node must insert after the current selection"
    );
    require(selected == 1 && grade_stack.grade_nodes.at(1) == duplicate,
            "insert must preserve every Grade Node and Render Op identity plus its payload");
    require(
        grade_stack.grade_nodes.at(0).tone_curve_render_op_id
            != grade_stack.grade_nodes.at(1).tone_curve_render_op_id,
        "a duplicate must reserve a fresh Tone Curve identity even before use"
    );
    require(
        grade_stack.grade_nodes.at(0).selective_tone_render_op_id
                != grade_stack.grade_nodes.at(1).selective_tone_render_op_id
            && grade_stack.grade_nodes.at(0).perceptual_color_render_op_id
                != grade_stack.grade_nodes.at(1).perceptual_color_render_op_id
            && grade_stack.grade_nodes.at(0).sharpen_render_op_id
                != grade_stack.grade_nodes.at(1).sharpen_render_op_id,
        "a duplicate must reserve fresh identities for every fine-edit render operation"
    );
    require(
        !GradeNodeStack::insertAfterSelection(grade_stack, duplicate, selected),
        "a duplicate persistent Grade Node identity must be rejected"
    );
}

void deletion_keeps_one_grade_node_and_selects_the_nearest_survivor() {
    BackendGradeStack grade_stack{
        .grade_nodes = {
            grade_node(QStringLiteral("first")),
            grade_node(QStringLiteral("second")),
            grade_node(QStringLiteral("third")),
        },
    };
    int selected = 2;
    require(
        GradeNodeStack::deleteSelection(grade_stack, selected),
        "selected Grade Node must delete"
    );
    require(
        selected == 1
            && grade_stack.grade_nodes.at(1).grade_node_id
                == QStringLiteral("second-grade-node"),
        "deleting the tail must select its nearest surviving neighbor"
    );
    require(
        GradeNodeStack::deleteSelection(grade_stack, selected),
        "a second Grade Node may delete"
    );
    require(
        !GradeNodeStack::deleteSelection(grade_stack, selected),
        "the final executable Grade Node must not be deleted"
    );
}

void reorder_and_selection_resolution_follow_stable_ids() {
    BackendGradeStack grade_stack{
        .grade_nodes = {
            grade_node(QStringLiteral("first")),
            grade_node(QStringLiteral("second")),
            grade_node(QStringLiteral("third")),
        },
    };
    int selected = 1;
    require(
        GradeNodeStack::moveSelection(grade_stack, selected, 0),
        "selected Grade Node must move"
    );
    require(
        selected == 0
            && grade_stack.grade_nodes.at(0).grade_node_id
                == QStringLiteral("second-grade-node"),
        "moving changes order without changing the selected identity"
    );
    require(
        GradeNodeStack::resolvedSelection(
            grade_stack,
            QStringLiteral("third-grade-node"),
            0
        ) == 2,
        "selection restoration must prefer the stable Grade Node identity"
    );
}

void insertion_is_bounded_to_sixteen_grade_nodes() {
    BackendGradeStack grade_stack{
        .grade_nodes = {grade_node(QStringLiteral("grade-node-1"))},
    };
    int selected = 0;
    for (int index = 2; index <= GradeNodeStack::maximum_grade_node_count; ++index) {
        require(
            GradeNodeStack::insertAfterSelection(
                grade_stack,
                grade_node(QStringLiteral("grade-node-%1").arg(index)),
                selected
            ),
            "every Grade Node through the desktop bound must insert"
        );
    }
    require(
        !GradeNodeStack::insertAfterSelection(
            grade_stack,
            grade_node(QStringLiteral("overflow")),
            selected
        ),
        "the seventeenth Grade Node must be rejected before crossing the bridge"
    );
}

void full_stack_undo_restores_every_persistent_identity() {
    BackendGradeStack before{
        .grade_nodes = {
            grade_node(QStringLiteral("first")),
            grade_node(QStringLiteral("second")),
        },
    };
    before.grade_nodes[1].tone_curve_kind = ToneCurveKind::SmoothRgb;
    before.grade_nodes[1].tone_curve_master_points = {{0.0, 0.1}, {1.0, 0.9}};
    BackendGradeStack after = before;
    int selected = 1;
    require(GradeNodeStack::moveSelection(after, selected, 0), "test move must succeed");
    after.grade_nodes[0].enabled = false;

    SessionEditHistory<BackendGradeStack> history;
    history.record("grade_node/second-grade-node/move", before, after);
    std::string restored_key;
    const auto restored = history.undo(after, &restored_key);
    require(restored && *restored == before, "undo must restore the exact complete stack");
    require(
        restored_key == "grade_node/second-grade-node/move",
        "undo must return the stable operation subject for UI reselection"
    );
    const auto redone = history.redo(*restored, &restored_key);
    require(redone && *redone == after, "redo must restore order, bypass, curves, and IDs");
}

void grade_node_reset_is_complete_and_one_undo_restores_everything() {
    BackendGradeStack before{
        .grade_nodes = {grade_node(QStringLiteral("creative"))},
    };
    auto& configured = before.grade_nodes[0];
    configured.basic = {
        .exposure_stops = 1.25,
        .contrast_factor = 1.4,
        .white_balance_temperature = 0.1,
        .white_balance_tint = -0.05,
        .saturation_factor = 1.2,
    };
    configured.enabled = false;
    configured.fine.highlights = -0.45;
    configured.fine.shadows = 0.3;
    configured.fine.vibrance = 0.25;
    configured.fine.mixer_hue[2] = -0.18;
    configured.fine.mixer_lightness[6] = 0.33;
    configured.fine.color_range_enabled = true;
    configured.fine.color_range_center = 212.0;
    configured.fine.color_range_width = 36.0;
    configured.fine.color_range_hue = 14.0;
    configured.fine.sharpen_amount = 1.1;
    configured.fine.sharpen_radius = 2.2;
    configured.fine.sharpen_threshold = 0.09;
    configured.fine.sharpen_masking = 0.45;
    configured.tone_curve_kind = ToneCurveKind::SmoothRgb;
    configured.tone_curve_master_points = {
        {0.0, 0.0}, {0.5, 0.65}, {1.0, 1.0}
    };
    configured.tone_curve_red_points = {{0.0, 0.0}, {1.0, 1.0}};
    configured.tone_curve_green_points = {{0.0, 0.0}, {1.0, 0.85}};
    configured.tone_curve_blue_points = {{0.0, 0.0}, {1.0, 1.0}};

    BackendGradeStack reset = before;
    require(
        GradeNodeStack::resetSelection(reset, 0),
        "reset must work on a bypassed Grade Node without changing bypass state"
    );
    const auto& neutral = reset.grade_nodes[0];
    require(
        neutral.basic == BackendBasicEditParameters{}
            && neutral.fine == BackendFineEditParameters{}
            && neutral.tone_curve_kind == ToneCurveKind::None
            && neutral.tone_curve_master_points.isEmpty()
            && neutral.tone_curve_red_points.isEmpty()
            && neutral.tone_curve_green_points.isEmpty()
            && neutral.tone_curve_blue_points.isEmpty() && !neutral.enabled
            && neutral.grade_node_id == configured.grade_node_id
            && neutral.tone_curve_render_op_id == configured.tone_curve_render_op_id
            && neutral.selective_tone_render_op_id
                == configured.selective_tone_render_op_id
            && neutral.perceptual_color_render_op_id
                == configured.perceptual_color_render_op_id
            && neutral.sharpen_render_op_id == configured.sharpen_render_op_id,
        "reset must clear Basic, fine-color, sharpening, and curve adjustments while preserving identity"
    );
    require(
        !GradeNodeStack::resetSelection(reset, 0),
        "resetting an already neutral Grade Node must be a no-op"
    );

    SessionEditHistory<BackendGradeStack> history;
    history.record("grade_node/creative-grade-node/reset", before, reset);
    const auto restored = history.undo(reset);
    require(
        restored && *restored == before && !history.canUndo(),
        "one session Undo step must restore Basic parameters and Tone Curve atomically"
    );
}

} // namespace

int main() {
    insertion_and_duplicate_identity_are_lossless();
    deletion_keeps_one_grade_node_and_selects_the_nearest_survivor();
    reorder_and_selection_resolution_follow_stable_ids();
    insertion_is_bounded_to_sixteen_grade_nodes();
    full_stack_undo_restores_every_persistent_identity();
    grade_node_reset_is_complete_and_one_undo_restores_everything();
    return EXIT_SUCCESS;
}
