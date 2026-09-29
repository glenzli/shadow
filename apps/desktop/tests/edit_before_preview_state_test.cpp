#include "edit_before_preview_state.hpp"

#include <cstdlib>
#include <iostream>

namespace {
bool require(const bool ok, const char* message) {
    if (!ok)
        std::cerr << message << '\n';
    return ok;
}
}

int main() {
    BackendGradeStack authored;
    authored.foundation.raw_white_balance_mode = 1;
    authored.foundation.temperature_kelvin = 7200;
    authored.raw_ai_denoise.present = true;
    authored.raw_ai_denoise.enabled = true;
    authored.foundation.optics.manual_distortion = 14;
    authored.geometry.present = true;
    authored.geometry.crop_left = 0.1;
    authored.geometry.crop_right = 0.7;
    authored.geometry.quarter_turn = 1;
    authored.geometry.perspective_vertical = 0.08;
    authored.grade_nodes.push_back(BackendGradeNode{});
    authored.retouch_spots.push_back(BackendRetouchSpot{});
    authored.image_completions.push_back(BackendImageCompletionRegion{});
    authored.liquify_strokes.push_back(BackendLiquifyStroke{});

    const auto baseline = neutral_before_stack(authored, authored.geometry, false);
    if (!require(baseline.geometry == authored.geometry
                     && baseline.foundation.optics == authored.foundation.optics
                     && baseline.foundation.raw_white_balance_mode == 0
                     && !baseline.raw_ai_denoise.present && baseline.grade_nodes.isEmpty()
                     && baseline.retouch_spots.isEmpty() && baseline.image_completions.isEmpty()
                     && baseline.liquify_strokes.isEmpty(),
                 "Before must retain framing/optics and exclude authored grading/AI/repairs"))
        return EXIT_FAILURE;

    EditBeforePreviewState state;
    if (!require(state.observe(1, baseline) && state.revision() == 1,
                 "first photo issues a comparison generation"))
        return EXIT_FAILURE;
    authored.foundation.temperature_kelvin = 4300;
    authored.grade_nodes.clear();
    if (!require(!state.observe(1, neutral_before_stack(authored, authored.geometry, false))
                     && state.revision() == 1,
                 "ordinary grading and RAW white balance reuse the original baseline"))
        return EXIT_FAILURE;
    authored.geometry.quarter_turn = 2;
    if (!require(state.observe(1, neutral_before_stack(authored, authored.geometry, false))
                     && state.revision() == 2,
                 "rotation invalidates a previous baseline"))
        return EXIT_FAILURE;
    authored.foundation.optics.correct_distortion = false;
    if (!require(state.observe(1, neutral_before_stack(authored, authored.geometry, false)),
                 "optics changes invalidate a previous baseline"))
        return EXIT_FAILURE;

    const auto full_crop_canvas = neutral_before_stack(authored, authored.geometry, true);
    if (!require(full_crop_canvas.geometry.crop_left == 0
                     && full_crop_canvas.geometry.crop_right == 1
                     && full_crop_canvas.geometry.quarter_turn == 2,
                 "the crop tool compares the complete oriented source"))
        return EXIT_FAILURE;
    (void)state.observe(1, full_crop_canvas);
    authored.geometry.crop_left = 0.3;
    if (!require(!state.observe(1, neutral_before_stack(authored, authored.geometry, true)),
                 "crop handle gestures do not rebuild the complete-source baseline"))
        return EXIT_FAILURE;
    if (!require(state.observe(1, neutral_before_stack(authored, authored.geometry, false))
                     && state.observe(2, neutral_before_stack(authored, authored.geometry, false)),
                 "leaving crop and changing photos each issue a new baseline identity"))
        return EXIT_FAILURE;
    return EXIT_SUCCESS;
}
