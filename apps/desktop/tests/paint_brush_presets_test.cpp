#include "paint_brush_presets.hpp"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <cstdlib>
#include <iostream>
#include <limits>
namespace {
void check(bool value, const char* message) {
    if (!value) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
} // namespace
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    check(dir.isValid(), "temporary preferences directory");
    const auto file = dir.filePath("brush.ini");
    PaintBrushProfile saved;
    QString id;
    {
        PaintBrushPresets p(file);
        check(p.presets().size() == 6, "six photographic presets");
        p.selectSlot(1);
    }
    {
        PaintBrushPresets p(file);
        check(p.slot() == 1 && p.current().preset_id == "dodge", "first A/B switch persists");
        p.selectSlot(0);
        auto profile = p.current();
        profile.stroke.roundness = 0.25;
        profile.stroke.angle_degrees = 37;
        profile.stroke.spacing = 0.07;
        profile.stroke.texture = 2;
        profile.stroke.texture_strength = 0.8;
        profile.stroke.pressure_size = true;
        profile.stroke.pressure_flow = false;
        profile.smoothing = 0.7;
        p.setCurrent(profile);
        check(p.save(QStringLiteral("  Local finishing  ")), "save custom brush");
        saved = p.current();
        id = saved.preset_id;
        p.selectSlot(1);
        check(p.current().preset_id == "dodge", "B independent of A");
        p.selectSlot(0);
        check(p.current() == saved, "A retains all tip parameters");
        check(!p.save(" ") && !p.save(QString(65, 'x')), "bounded preset name");
        check(
            p.apply("burn") && p.current().blend == 2 && p.current().stroke.red == 0,
            "burn starts with dark luminance brush"
        );
        check(p.apply(id) && p.current() == saved, "custom restores all values");
        profile = p.current();
        profile.stroke.spacing = std::numeric_limits<double>::quiet_NaN();
        p.setCurrent(profile);
        check(p.current() == saved, "invalid preference rejected");
        profile = saved;
        profile.stroke.points.push_back({0.3, 0.4, 0.5});
        p.setCurrent(profile);
        check(p.current() == saved, "photo point data never persists in preferences");
    }
    {
        PaintBrushPresets p(file);
        check(p.current() == saved && p.presets().size() == 7, "reopen custom and A/B state");
        check(
            p.save("Local finishing") && p.presets().size() == 7,
            "same name replaces without duplicate"
        );
        for (int i = 0; i < 11; ++i)
            check(p.save(QStringLiteral("Brush %1").arg(i)), "save bounded collection");
        check(!p.save("overflow") && p.presets().size() == 18, "custom collection bound");
        check(p.remove(id) && !p.apply(id) && !p.remove("dodge"), "remove only custom brushes");
    }
    {
        QSettings corrupt(dir.filePath("corrupt.ini"), QSettings::IniFormat);
        corrupt.setValue("version", 1);
        corrupt.setValue("slot0", QByteArray("{bad json"));
        corrupt.setValue("custom", QByteArray(65537, 'x'));
        corrupt.sync();
        PaintBrushPresets p(corrupt.fileName());
        check(
            p.current().preset_id == "soft-color" && p.presets().size() == 6,
            "malformed or excessive preference data falls back safely"
        );
    }
    std::cout << "Brush presets: persistence, isolation, validation and bounds passed\n";
}
