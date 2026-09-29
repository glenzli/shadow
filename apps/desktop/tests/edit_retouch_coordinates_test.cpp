#include "edit_retouch_coordinates.hpp"
#include <cmath>
#include <iostream>
#include <shadow/image/photo_geometry.hpp>
#include <shadow/image/working_rgb.hpp>

int main() {
    BackendGradeStack stack;
    EditRetouchCoordinates map;
    const auto check = [&](bool ok, const char* message) {
        if (!ok)
            std::cerr << message << '\n';
        return ok;
    };
    stack.geometry.crop_left = 0.1;
    stack.geometry.crop_right = 0.9;
    stack.geometry.crop_top = 0.2;
    stack.geometry.crop_bottom = 0.8;
    stack.geometry.quarter_turn = 1;
    if (!check(map.update(stack, {1000, 800}), "could not map crop and rotation"))
        return 1;
    const auto original = map.original({0.2, 0.3});
    if (!check(
            original && std::hypot(original->x() - 0.34, original->y() - 0.68) < 1e-9,
            "rotated crop wrote viewport coordinates"
        ))
        return 1;
    stack.geometry.straighten_degrees = 13;
    stack.geometry.perspective_vertical = 0.35;
    stack.geometry.perspective_horizontal = -0.24;
    stack.liquify_enabled = true;
    stack.liquify_strokes = {
        {.points = {{0.4, 0.5, 1}, {0.43, 0.53, 1}},
         .radius = 0.15,
         .strength = 0.55,
         .hardness = 0.3}
    };
    for (int turn = 0; turn < 4; ++turn) {
        stack.geometry.quarter_turn = static_cast<std::uint8_t>(turn);
        stack.geometry.flip_horizontal = turn % 2 == 0;
        stack.geometry.flip_vertical = turn % 2 == 1;
        if (!check(map.update(stack, {1000, 800}), "combined mapping failed"))
            return 1;
        for (int y = 1; y < 10; ++y)
            for (int x = 1; x < 10; ++x) {
                const QPointF view(x / 10.0, y / 10.0);
                const auto p = map.original(view);
                const auto q = p ? map.preview(*p) : std::nullopt;
                if (!check(
                        q && std::hypot(q->x() - view.x(), q->y() - view.y()) < 0.0001,
                        "donor/target coordinate roundtrip drift"
                    ))
                    return 1;
            }
    }
    stack.geometry.enabled = false;
    stack.liquify_enabled = false;
    if (!check(map.update(stack, {1000, 800}), "bypass mapping failed"))
        return 1;
    const auto identity = map.original({0.2, 0.3});
    if (!check(
            identity && std::hypot(identity->x() - 0.2, identity->y() - 0.3) < 1e-12,
            "bypassed geometry retained stale mapping"
        ))
        return 1;
    if (!check(!map.update(stack, {}), "invalid extent was accepted"))
        return 1;

    namespace image = shadow::image;
    image::FloatRgbImage gradient{
        .dimensions = {501, 399},
        .row_stride_bytes = 501 * 3 * sizeof(float),
        .pixel_format = image::FloatPixelFormat::rgb_f32_native_interleaved,
        .transfer_function = image::TransferFunction::linear
    };
    gradient.samples.resize(501 * 399 * 3);
    for (int y = 0; y < 399; ++y)
        for (int x = 0; x < 501; ++x) {
            gradient.samples[(y * 501 + x) * 3] = float((x + 0.5) / 501);
            gradient.samples[(y * 501 + x) * 3 + 1] = float((y + 0.5) / 399);
        }
    stack.geometry.enabled = true;
    for (int turn = 0; turn < 4; ++turn) {
        stack.geometry.quarter_turn = static_cast<std::uint8_t>(turn);
        image::PhotoGeometry geometry{
            .crop_left = 0.1,
            .crop_top = 0.2,
            .crop_right = 0.9,
            .crop_bottom = 0.8,
            .quarter_turn = static_cast<image::PhotoQuarterTurn>(turn),
            .straighten_degrees = 13,
            .perspective_vertical = 0.35,
            .perspective_horizontal = -0.24,
            .flip_horizontal = stack.geometry.flip_horizontal,
            .flip_vertical = stack.geometry.flip_vertical
        };
        if (!map.update(stack, {501, 399}))
            return 1;
        const auto rendered = image::apply_photo_geometry(gradient, geometry);
        for (std::uint32_t y = 8; y + 8 < rendered.dimensions.height; y += 17)
            for (std::uint32_t x = 8; x + 8 < rendered.dimensions.width; x += 19) {
                const auto p = map.original(
                    {(x + 0.5) / rendered.dimensions.width, (y + 0.5) / rendered.dimensions.height}
                );
                const auto i = (y * rendered.dimensions.width + x) * 3;
                if (!check(
                        p && std::abs(p->x() - rendered.samples[i]) < 2e-6
                            && std::abs(p->y() - rendered.samples[i + 1]) < 2e-6,
                        "authoring coordinate differs from rendered gradient pixel"
                    ))
                    return 1;
            }
    }
    return 0;
}
