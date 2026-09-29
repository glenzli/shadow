#include "edit_controller.hpp"
#include "edit_paint_controller.hpp"
#include <algorithm>

// Dedicated photographic light sculpting reuses the established Oklab-L soft-light
// recipe. No new blend math, schema, source decode or full-frame brush buffer.
bool EditPaintController::isDodgeBurnLayer(const BackendPaintLayer& layer) {
    return layer.blend == 2 && std::ranges::all_of(layer.strokes, [](const auto& stroke) {
               return stroke.erase || (stroke.red == stroke.green && stroke.green == stroke.blue);
           });
}
void EditPaintController::setDodgeBurn(bool value) {
    if (dodge_burn_ == value || strokeActive())
        return;
    owner_.finishActiveGesture();
    if (dodge_burn_)
        dodge_burn_brush_ = PaintBrushProfile{brush_, smoothing_, 2, {}};
    dodge_burn_ = value;
    if (value) {
        if (!dodge_burn_brush_) {
            PaintBrushProfile profile;
            profile.stroke.radius = 0.03;
            profile.stroke.flow = 0.05;
            profile.stroke.hardness = 0;
            profile.stroke.pressure_flow = true;
            profile.blend = 2;
            dodge_burn_brush_ = profile;
        }
        brush_ = dodge_burn_brush_->stroke;
        smoothing_ = dodge_burn_brush_->smoothing;
        default_blend_ = 2;
        brush_.red = brush_.green = brush_.blue = burn_ ? 0.0 : 1.0;
        brush_.erase = false;
    } else
        loadBrush();
    picking_ = false;
    status_.clear();
    emit brushChanged();
    emit changed();
}
void EditPaintController::setBurn(bool value) {
    if (!dodge_burn_ || strokeActive())
        return;
    burn_ = value;
    brush_.red = brush_.green = brush_.blue = value ? 0.0 : 1.0;
    saveBrush();
}
