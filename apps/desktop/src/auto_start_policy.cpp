#include "auto_start_policy.hpp"
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <vector>

namespace {
double percentile(std::vector<double> samples, double q) {
    if (samples.empty())
        return 0;
    const auto i = static_cast<std::size_t>(q * static_cast<double>(samples.size() - 1));
    std::nth_element(
        samples.begin(),
        samples.begin() + static_cast<std::ptrdiff_t>(i),
        samples.end()
    );
    return samples[i];
}
double linear(double v) {
    return v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4);
}
struct Lab {
    double l, c, h;
};
Lab lab(QRgb pixel) {
    const double r = linear(qRed(pixel) / 255.), g = linear(qGreen(pixel) / 255.),
                 b = linear(qBlue(pixel) / 255.);
    const double l = std::cbrt(.4122214708 * r + .5363325363 * g + .0514459929 * b);
    const double m = std::cbrt(.2119034982 * r + .6806995451 * g + .1073969566 * b);
    const double s = std::cbrt(.0883024619 * r + .2817188376 * g + .6299787005 * b);
    const double a = 1.9779984951 * l - 2.428592205 * m + .4505937099 * s;
    const double bb = .0259040371 * l + .7827717662 * m - .808675766 * s;
    return {
        .2104542553 * l + .793617785 * m - .0040720468 * s,
        std::hypot(a, bb),
        std::fmod(std::atan2(bb, a) * 180 / 3.141592653589793 + 360, 360)
    };
}
} // namespace
bool autoStartPreservesIlluminant(const QString& scene) {
    static const QRegularExpression lighting(
        QStringLiteral(
            "sunset|sunrise|golden.hour|blue.hour|candle|neon|stage.light|colored.light|colourful."
            "light|silhouette|night|夕阳|日落|日出|烛|霓虹|舞台|剪影|夜景"
        ),
        QRegularExpression::CaseInsensitiveOption
    );
    return lighting.match(scene).hasMatch();
}
AutoStartTone measureAutoStartTone(const QImage& input, const QString& scene) {
    AutoStartTone result;
    if (input.isNull())
        return result;
    const auto image = input.scaled(256, 256, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                           .convertToFormat(QImage::Format_RGB32);
    std::vector<double> values;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) {
            const auto p = image.pixel(x, y);
            values.push_back((.2126 * qRed(p) + .7152 * qGreen(p) + .0722 * qBlue(p)) / 255.);
        }
    const double p10 = percentile(values, .10), p50 = percentile(values, .50),
                 p95 = percentile(values, .95), p99 = percentile(values, .99);
    result.preserveLight = autoStartPreservesIlluminant(scene) || p50 < .10 || p50 > .82;
    if (result.preserveLight || p95 - p10 < .06)
        return result;
    double ev = .55 * std::log2(linear(.45) / std::max(.001, linear(p50)));
    // A bright background is not evidence of overexposure. Leave the broad
    // normal range alone; reduce exposure only when bright midtones coincide
    // with near-clipped highlights.
    if (p50 >= .35 && (p50 <= .72 || p95 < .97))
        ev = 0;
    ev = std::clamp(ev, -.30, .65);
    if (p99 > .98 && ev > 0)
        ev = 0;
    if (std::abs(ev) < .08)
        ev = 0;
    result.basic.exposure_stops = ev;
    result.fine.highlights = p95 > .88 ? -std::clamp((p95 - .88) * 1.8, 0., .22) : 0;
    result.fine.shadows = p10 < .16 && p50 > .18 ? std::clamp((.16 - p10) * .8, 0., .10) : 0;
    // Preserve flat/foggy scenes: no automatic black-point or clarity stretch.
    result.useful =
        std::abs(ev) > .01 || std::abs(result.fine.highlights) > .025 || result.fine.shadows > .025;
    return result;
}
AutoStartTone
measureAutoStartSkin(const QImage& input, const QByteArray& coverage, int width, int height) {
    AutoStartTone result;
    if (input.isNull() || width <= 0 || height <= 0 || width > 1024 || height > 1024
        || coverage.size() != width * height)
        return result;
    const auto image = input.scaled(width, height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                           .convertToFormat(QImage::Format_RGB32);
    std::vector<double> hues;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            if (static_cast<unsigned char>(coverage[y * width + x]) < 224)
                continue;
            auto p = lab(image.pixel(x, y));
            // Only visible midtone skin provides colour evidence. Preserve unusual
            // illumination and deep shadows instead of forcing a universal skin hue.
            if (p.l < .25 || p.l > .88 || p.c < .025 || p.c > .24 || p.h < 15 || p.h > 85)
                continue;
            hues.push_back(p.h);
        }
    if (hues.size() < 96)
        return result;
    const double center = percentile(hues, .5), low = percentile(hues, .2),
                 high = percentile(hues, .8);
    if (high - low > 30)
        return result;
    const auto range = [&](double h) {
        return BackendPointColorRange{
            .enabled = true,
            .center_degrees = h,
            .width_degrees = std::min(6., std::abs(h - center) * .7),
            .softness = .75,
            .hue_shift_degrees = std::clamp((center - h) * .35, -4., 4.),
            .saturation = 0,
            .lightness = 0
        };
    };
    if (center - low > 5)
        result.fine.additional_point_colors.push_back(range(low));
    if (high - center > 5)
        result.fine.additional_point_colors.push_back(range(high));
    // The Point Color wire format keeps its first range in the scalar fields.
    if (!result.fine.additional_point_colors.isEmpty()) {
        auto first = result.fine.additional_point_colors.takeFirst();
        result.fine.color_range_enabled = true;
        result.fine.color_range_center = first.center_degrees;
        result.fine.color_range_width = first.width_degrees;
        result.fine.color_range_softness = first.softness;
        result.fine.color_range_hue = first.hue_shift_degrees;
    }
    result.useful = result.fine.color_range_enabled;
    return result;
}

double autoStartDisplayClippedFraction(const QImage& input) {
    if (input.isNull())
        return 0;
    const auto image = input.scaled(256, 256, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                           .convertToFormat(QImage::Format_RGB32);
    int clipped = 0;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) {
            const auto p = image.pixel(x, y);
            if (std::max({qRed(p), qGreen(p), qBlue(p)}) >= 253)
                ++clipped;
        }
    return double(clipped) / double(image.width() * image.height());
}
