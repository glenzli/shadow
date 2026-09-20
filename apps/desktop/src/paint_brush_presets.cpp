#include "paint_brush_presets.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QUuid>
#include <cmath>
#include <optional>

namespace {
QJsonObject encode(const PaintBrushProfile& p) {
    const auto& b = p.stroke;
    return {
        {"radius", b.radius},
        {"hardness", b.hardness},
        {"opacity", b.opacity},
        {"flow", b.flow},
        {"red", b.red},
        {"green", b.green},
        {"blue", b.blue},
        {"erase", b.erase},
        {"roundness", b.roundness},
        {"angle", b.angle_degrees},
        {"spacing", b.spacing},
        {"texture", b.texture},
        {"textureStrength", b.texture_strength},
        {"pressureSize", b.pressure_size},
        {"pressureFlow", b.pressure_flow},
        {"smoothing", p.smoothing},
        {"blend", p.blend},
        {"preset", p.preset_id}
    };
}
std::optional<PaintBrushProfile> decode(const QJsonObject& o) {
    for (const auto key :
         {"radius",
          "hardness",
          "opacity",
          "flow",
          "red",
          "green",
          "blue",
          "roundness",
          "angle",
          "spacing",
          "texture",
          "textureStrength",
          "smoothing",
          "blend"})
        if (!o.value(QLatin1String(key)).isDouble())
            return {};
    for (const auto key : {"erase", "pressureSize", "pressureFlow"})
        if (!o.value(QLatin1String(key)).isBool())
            return {};
    const auto value = [&](const char* k) { return o.value(QLatin1String(k)).toDouble(); };
    if (value("texture") != std::floor(value("texture")) || value("texture") < 0
        || value("texture") > 2 || value("blend") != std::floor(value("blend"))
        || value("blend") < 0 || value("blend") > 2)
        return {};
    PaintBrushProfile p;
    auto& b = p.stroke;
    b.radius = value("radius");
    b.hardness = value("hardness");
    b.opacity = value("opacity");
    b.flow = value("flow");
    b.red = value("red");
    b.green = value("green");
    b.blue = value("blue");
    b.erase = o["erase"].toBool();
    b.roundness = value("roundness");
    b.angle_degrees = value("angle");
    b.spacing = value("spacing");
    b.texture = static_cast<std::uint8_t>(value("texture"));
    b.texture_strength = value("textureStrength");
    b.pressure_size = o["pressureSize"].toBool();
    b.pressure_flow = o["pressureFlow"].toBool();
    p.smoothing = value("smoothing");
    p.blend = static_cast<int>(value("blend"));
    p.preset_id = o["preset"].toString();
    return PaintBrushPresets::valid(p) ? std::optional{p} : std::nullopt;
}
QString label(const QString& id) {
    if (id == "soft-color")
        return QCoreApplication::translate("PaintBrushPresets", "Soft color repair");
    if (id == "dodge")
        return QCoreApplication::translate("PaintBrushPresets", "Gentle dodge");
    if (id == "burn")
        return QCoreApplication::translate("PaintBrushPresets", "Gentle burn");
    if (id == "detail")
        return QCoreApplication::translate("PaintBrushPresets", "Fine edge");
    if (id == "erase")
        return QCoreApplication::translate("PaintBrushPresets", "Soft erase");
    return QCoreApplication::translate("PaintBrushPresets", "Fine texture");
}
const QStringList builtin_ids{"soft-color", "dodge", "burn", "detail", "erase", "texture"};
PaintBrushProfile builtin(const QString& id, const PaintBrushProfile& previous) {
    PaintBrushProfile p;
    p.preset_id = id;
    p.stroke.red = previous.stroke.red;
    p.stroke.green = previous.stroke.green;
    p.stroke.blue = previous.stroke.blue;
    p.stroke.pressure_size = previous.stroke.pressure_size;
    p.stroke.pressure_flow = previous.stroke.pressure_flow;
    if (id == "dodge" || id == "burn") {
        p.blend = 2;
        p.stroke.radius = 0.035;
        p.stroke.flow = 0.06;
        p.stroke.red = p.stroke.green = p.stroke.blue = (id == "dodge" ? 1.0 : 0.0);
    } else if (id == "detail") {
        p.blend = 0;
        p.stroke.radius = 0.003;
        p.stroke.hardness = 0.85;
        p.stroke.flow = 0.5;
        p.smoothing = 0.4;
    } else if (id == "erase") {
        p.blend = previous.blend;
        p.stroke.erase = true;
        p.stroke.flow = 0.15;
        p.stroke.radius = 0.025;
    } else if (id == "texture") {
        p.stroke.texture = 1;
        p.stroke.radius = 0.015;
        p.stroke.hardness = 0.2;
        p.stroke.flow = 0.12;
    }
    return p;
}
} // namespace
QString PaintBrushPresets::defaultSettingsFile() {
    if (const auto path = qEnvironmentVariable("SHADOW_PAINT_BRUSH_SETTINGS_FILE"); !path.isEmpty())
        return path;
    QString root = qEnvironmentVariable("SHADOW_DESKTOP_DATA_ROOT");
    if (qEnvironmentVariableIsSet("SHADOW_PIPELINE_SMOKE_ACTION"))
        root = QDir::tempPath() + "/shadow-paint-smoke-"
               + QString::number(QCoreApplication::applicationPid());
    if (root.isEmpty())
        root = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return QDir(root).filePath(QStringLiteral("paint-brushes.ini"));
}
PaintBrushPresets::PaintBrushPresets(const QString& file) : settings_(file, QSettings::IniFormat) {
    slots_[0] = builtin("soft-color", {});
    slots_[1] = builtin("dodge", {});
    if (settings_.value("version").toInt() == 1) {
        for (int i = 0; i < 2; ++i) {
            const auto bytes = settings_.value(QStringLiteral("slot%1").arg(i)).toByteArray();
            if (bytes.size() > 8192)
                continue;
            if (auto p = decode(QJsonDocument::fromJson(bytes).object()))
                slots_[i] = *p;
        }
        slot_ = settings_.value("active").toInt() == 1 ? 1 : 0;
    }
    loadCustom();
}
const PaintBrushProfile& PaintBrushPresets::current() const {
    return slots_[slot_];
}
bool PaintBrushPresets::valid(const PaintBrushProfile& p) {
    const auto within = [](double x, double a, double b) {
        return std::isfinite(x) && x >= a && x <= b;
    };
    const auto& s = p.stroke;
    return s.points.isEmpty() && within(s.radius, 0.0001, 0.25) && within(s.hardness, 0, 1)
           && within(s.opacity, 0, 1) && within(s.flow, 0.01, 1) && within(s.red, 0, 1)
           && within(s.green, 0, 1) && within(s.blue, 0, 1) && within(s.roundness, 0.1, 1)
           && within(s.angle_degrees, -180, 180) && within(s.spacing, 0.02, 1) && s.texture <= 2
           && within(s.texture_strength, 0, 1) && within(p.smoothing, 0, 1) && p.blend >= 0
           && p.blend <= 2 && p.preset_id.size() <= 80;
}
void PaintBrushPresets::setCurrent(PaintBrushProfile p) {
    p.stroke.points.clear();
    if (!valid(p))
        return;
    slots_[slot_] = std::move(p);
    settings_.setValue("version", 1);
    settings_.setValue(
        QStringLiteral("slot%1").arg(slot_),
        QJsonDocument(encode(current())).toJson(QJsonDocument::Compact)
    );
}
void PaintBrushPresets::selectSlot(int slot) {
    if (slot < 0 || slot > 1)
        return;
    settings_.setValue("version", 1);
    slot_ = slot;
    settings_.setValue("active", slot_);
}
void PaintBrushPresets::loadCustom() {
    custom_.clear();
    const auto bytes = settings_.value("custom").toByteArray();
    if (bytes.size() > 65536)
        return;
    const auto array = QJsonDocument::fromJson(bytes).array();
    for (const auto& entry : array) {
        const auto o = entry.toObject();
        const auto id = o["id"].toString(), name = o["name"].toString();
        if (custom_.size() >= 12)
            break;
        if (id.isEmpty() || id.size() > 80 || builtin_ids.contains(id) || name.trimmed().isEmpty()
            || name.size() > 64)
            continue;
        if (auto p = decode(o["profile"].toObject()))
            custom_.push_back({id, name, *p});
    }
}
QVariantList PaintBrushPresets::presets() const {
    QVariantList result;
    for (const auto& id : builtin_ids)
        result.push_back(QVariantMap{{"id", id}, {"label", label(id)}, {"custom", false}});
    for (const auto& p : custom_)
        result.push_back(QVariantMap{{"id", p.id}, {"label", p.label}, {"custom", true}});
    return result;
}
bool PaintBrushPresets::apply(const QString& id) {
    if (builtin_ids.contains(id)) {
        setCurrent(builtin(id, current()));
        return true;
    }
    for (const auto& p : custom_)
        if (p.id == id) {
            setCurrent(p.profile);
            return true;
        }
    return false;
}
void PaintBrushPresets::saveCustom() {
    QJsonArray array;
    for (const auto& p : custom_)
        array.push_back(
            QJsonObject{{"id", p.id}, {"name", p.label}, {"profile", encode(p.profile)}}
        );
    settings_.setValue("custom", QJsonDocument(array).toJson(QJsonDocument::Compact));
}
bool PaintBrushPresets::save(const QString& input) {
    const auto name = input.trimmed();
    if (name.isEmpty() || name.size() > 64)
        return false;
    settings_.sync();
    loadCustom();
    auto p = current();
    for (auto& entry : custom_)
        if (entry.label == name) {
            p.preset_id = entry.id;
            entry.profile = p;
            saveCustom();
            setCurrent(p);
            return true;
        }
    if (custom_.size() >= 12)
        return false;
    p.preset_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    custom_.push_back({p.preset_id, name, p});
    saveCustom();
    setCurrent(p);
    return true;
}
bool PaintBrushPresets::remove(const QString& id) {
    settings_.sync();
    loadCustom();
    for (qsizetype i = 0; i < custom_.size(); ++i)
        if (custom_[i].id == id) {
            custom_.removeAt(i);
            saveCustom();
            return true;
        }
    return false;
}
