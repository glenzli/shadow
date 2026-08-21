#pragma once

#include <shadow/image/decoder_types.hpp>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace shadow::image {

// This is an in-process CPU buffer, not a persistence or public ABI format. Samples are
// native-endian IEEE-754 binary32 values in interleaved R, G, B order.
enum class FloatPixelFormat : std::uint8_t {
    unknown,
    rgb_f32_native_interleaved,
};

enum class TransferFunction : std::uint8_t {
    unknown,
    linear,
};

enum class ImageReference : std::uint8_t {
    unknown,
    // Relative processed scene-referred RGB: values remain linear-light and no display OETF or
    // look/tone rendering has been applied, but camera black subtraction, white balance,
    // demosaic, color-matrix conversion, normalization, and highlight clipping may already have
    // occurred. This must not be interpreted as sensor-linear mosaic/radiance data.
    scene_referred,
    // An ordinary rendered source (JPEG/SDR HEIF) that has been colour-managed and transfer
    // decoded into linear working RGB. It is linear for the purpose of composable adjustments,
    // but its original appearance is already display-referred; the output boundary therefore
    // must apply gamut mapping and the sRGB OETF only, rather than Shadow's RAW scene curve.
    display_referred,
};

struct Chromaticity final {
    double x = 0.0;
    double y = 0.0;

    auto operator<=>(const Chromaticity&) const = default;
};

// The luminance coefficients are the Y row of the working-RGB-to-XYZ matrix. Keeping
// them beside the primaries makes saturation independent of any hard-coded working space.
struct WorkingRgbSpace final {
    std::string id;
    std::array<Chromaticity, 3> primaries{};
    Chromaticity white_point;
    std::array<double, 3> luminance_coefficients{};

    auto operator<=>(const WorkingRgbSpace&) const = default;
};

struct FloatRgbImage final {
    Dimensions dimensions;
    std::size_t row_stride_bytes = 0;
    FloatPixelFormat pixel_format = FloatPixelFormat::unknown;
    TransferFunction transfer_function = TransferFunction::unknown;
    ImageReference reference = ImageReference::unknown;
    WorkingRgbSpace working_space;
    // The raster's sampling density relative to level-0/full-resolution pixels. A full-detail
    // image is 1x1; a 1/4-size warm proxy is approximately 0.25x0.25. Spatial operations use
    // these values to keep their public radius expressed in level-0 pixels.
    double level_zero_to_raster_scale_x = 1.0;
    double level_zero_to_raster_scale_y = 1.0;
    std::vector<float> samples;
};

} // namespace shadow::image
