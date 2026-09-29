#include "auto_start_policy.hpp"
#include <QColor>
#include <QCoreApplication>
#include <cmath>
#include <cstdlib>
#include <iostream>
namespace {
void check(bool ok, const char* message) {
    if (!ok) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
QImage ramp(int low, int high) {
    QImage image(256, 64, QImage::Format_RGB32);
    for (int x = 0; x < 256; ++x)
        for (int y = 0; y < 64; ++y) {
            int v = low + (high - low) * x / 255;
            image.setPixel(x, y, qRgb(v, v, v));
        }
    return image;
}
} // namespace
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    check(
        autoStartDisplayClippedFraction(ramp(0, 255)) > 0,
        "clipped endpoint measurement sees the bright tail"
    );
    const QVector<BackendSubjectMaskPerson> people{
        {.index = 0, .confidence = .4},
        {.index = 1, .confidence = .7},
        {.index = 2, .confidence = .95},
        {.index = 3, .confidence = .95}
    };
    check(
        autoStartSkinPeople(people, 2) == QVector<std::uint32_t>{2, 3},
        "uncertain early faces do not consume the automatic skin budget"
    );
    check(autoStartSkinPeople(people, 0).isEmpty(), "a full grade stack admits no skin nodes");
    const auto dim = measureAutoStartTone(ramp(25, 130));
    check(
        dim.useful && dim.basic.exposure_stops > 0 && dim.basic.exposure_stops <= .65,
        "underexposed scene gets a bounded lift"
    );
    check(
        measureAutoStartTone(ramp(115, 210)).basic.exposure_stops == 0,
        "bright backgrounds must not be normalized down to middle grey"
    );
    const auto night = measureAutoStartTone(ramp(0, 30));
    check(night.preserveLight && !night.useful, "night must not be normalized to middle grey");
    check(!measureAutoStartTone(ramp(115, 122)).useful, "flat fog does not get forced contrast");
    check(
        !measureAutoStartTone(ramp(25, 130), QStringLiteral("A portrait in golden hour light"))
             .useful,
        "intentional warm light stays intact"
    );
    auto clipped = ramp(20, 100);
    for (int y = 0; y < 64; ++y)
        for (int x = 245; x < 256; ++x)
            clipped.setPixel(x, y, qRgb(255, 255, 255));
    check(
        measureAutoStartTone(clipped).basic.exposure_stops <= 0,
        "display clipping prevents a positive global exposure proposal"
    );
    QImage skin(64, 64, QImage::Format_RGB32);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
            skin.setPixelColor(
                x,
                y,
                x < 20   ? QColor(195, 130, 114)
                : x > 44 ? QColor(195, 137, 105)
                         : QColor(195, 133, 109)
            );
    const QByteArray mask(4096, char(255));
    const auto correction = measureAutoStartSkin(skin, mask, 64, 64);
    check(correction.useful, "uneven skin hues produce a restrained local correction");
    check(
        std::abs(correction.fine.color_range_hue) <= 4 && correction.basic.exposure_stops == 0,
        "skin correction avoids whitening and large colour shifts"
    );
    check(
        !measureAutoStartSkin(skin, QByteArray(4096, char(0)), 64, 64).useful,
        "unselected skin never contributes evidence"
    );
    check(
        !measureAutoStartSkin(skin, QByteArray(4096, char(100)), 64, 64).useful,
        "uncertain edges never drive skin colour"
    );
    skin.fill(QColor(195, 133, 109));
    check(!measureAutoStartSkin(skin, mask, 64, 64).useful, "uniform skin needs no correction");
    check(!measureAutoStartSkin(skin, mask, 63, 64).useful, "malformed coverage fails closed");
    std::cout << "Auto start tone, lighting-intent and skin evidence contracts passed\n";
}
