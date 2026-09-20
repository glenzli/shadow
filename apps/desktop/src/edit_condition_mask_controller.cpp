#include "edit_condition_mask_controller.hpp"
#include "edit_controller.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cmath>

namespace {
bool validNode(const QJsonObject& node, int depth, int& leaves) {
    if (depth > 4)
        return false;
    const QString kind = node.value("operator").toString();
    if (kind == "not")
        return validNode(node.value("child").toObject(), depth + 1, leaves);
    if (kind == "all" || kind == "any") {
        const auto children = node.value("children").toArray();
        if (children.size() < 2 || children.size() > 4)
            return false;
        for (const auto& child : children)
            if (!validNode(child.toObject(), depth + 1, leaves))
                return false;
        return true;
    }
    if (kind != "leaf" || ++leaves > 8)
        return false;
    const auto c = node.value("condition").toObject();
    const auto number = [&](const char* key, double low, double high) {
        const auto v = c.value(QLatin1String(key));
        return v.isDouble() && std::isfinite(v.toDouble()) && v.toDouble() >= low
               && v.toDouble() <= high;
    };
    if (!number("softness", 0.0, 1.0))
        return false;
    const auto predicate = c.value("kind").toString();
    if (predicate == "oklab_lightness_range" || predicate == "oklch_chroma_range")
        return number("lower", 0, 1) && number("upper", 0, 1)
               && c.value("lower").toDouble() <= c.value("upper").toDouble();
    if (predicate == "oklch_hue_range")
        return number("center_hue_degrees", 0, 359.999999) && number("half_width_degrees", 1, 180)
               && number("minimum_chroma", 0, 1)
               && (!c.contains("minimum_chroma_feather") || number("minimum_chroma_feather", 0, 1))
               && (c.value("minimum_chroma").toDouble() != 0
                   || c.value("minimum_chroma_feather").toDouble() == 0);
    return false;
}
} // namespace

QString defaultConditionMaskExpression() {
    return QString::fromUtf8(
        QJsonDocument::fromJson(
            QByteArrayLiteral(
                R"({"schema_version":1,"root":{"operator":"all","children":[{"operator":"leaf","condition":{"kind":"oklab_lightness_range","lower":0.2,"upper":0.8,"softness":0.08}},{"operator":"leaf","condition":{"kind":"oklch_chroma_range","lower":0,"upper":1,"softness":0.05}}]}})"
            )
        )
            .toJson(QJsonDocument::Compact)
    );
}

QVariantMap EditController::selectedConditionMask() const {
    const auto* c = selectedLocalMaskComponent();
    if (c == nullptr)
        return {};
    if (c->kind == 7) {
        auto expression = QJsonDocument::fromJson(c->condition_expression.toUtf8()).object();
        if (c->leaf_invert)
            expression["root"] = QJsonObject{{"operator", "not"}, {"child", expression["root"]}};
        return expression.toVariantMap();
    }
    QJsonObject condition;
    if (c->kind == 4)
        condition = {
            {"kind", "oklab_lightness_range"},
            {"lower", c->x0},
            {"upper", c->x1},
            {"softness", c->feather}
        };
    else if (c->kind == 5)
        condition = {
            {"kind", "oklch_hue_range"},
            {"center_hue_degrees", c->x0 * 360.0},
            {"half_width_degrees", c->x1 * 180.0},
            {"softness", c->feather},
            {"minimum_chroma", 0.0},
            {"minimum_chroma_feather", 0.0}
        };
    else
        return {};
    QJsonObject root{{"operator", "leaf"}, {"condition", condition}};
    if (c->leaf_invert)
        root = {{"operator", "not"}, {"child", root}};
    return QJsonObject{{"schema_version", 1}, {"root", root}}.toVariantMap();
}

void EditController::setSelectedConditionMask(const QString& json) {
    auto* c = selectedLocalMaskComponent();
    const auto* grade = selectedGradeNode();
    if (!active_ || interactionLocked() || c == nullptr || grade == nullptr || !grade->enabled
        || (c->kind != 4 && c->kind != 5 && c->kind != 7))
        return;
    int leaves = 0;
    const auto doc = QJsonDocument::fromJson(json.toUtf8());
    if (json.size() > 16'384 || !doc.isObject() || doc.object().value("schema_version").toInt() != 1
        || !validNode(doc.object().value("root").toObject(), 1, leaves)) {
        setStatusMessage(
            {"EditController",
             QT_TRANSLATE_NOOP(
                 "EditController",
                 "Use up to 8 valid conditions in groups of 2–4, with at most 4 levels."
             ),
             {}}
        );
        return;
    }
    const auto compact = QString::fromUtf8(doc.toJson(QJsonDocument::Compact));
    if (c->kind == 7 && c->condition_expression == compact)
        return;
    const BackendGradeStack before = grade_stack_;
    const auto id = c->component_id;
    const auto operation = c->operation;
    const auto enabled = c->enabled;
    *c = BackendMaskComponent{};
    c->component_id = id;
    c->operation = operation;
    c->enabled = enabled;
    c->kind = 7;
    c->leaf_invert = false;
    c->condition_expression = compact;
    auto root = doc.object().value("root").toObject();
    bool inverse = false;
    while (root.value("operator").toString() == "not") {
        inverse = !inverse;
        root = root.value("child").toObject();
    }
    const auto predicate = root.value("condition").toObject();
    const auto kind = predicate.value("kind").toString();
    if (root.value("operator").toString() == "leaf"
        && (kind == "oklab_lightness_range"
            || (kind == "oklch_hue_range" && predicate.value("minimum_chroma").toDouble() == 0))) {
        c->kind = kind == "oklab_lightness_range" ? 4 : 5;
        c->x0 = c->kind == 4 ? predicate.value("lower").toDouble()
                             : predicate.value("center_hue_degrees").toDouble() / 360.0;
        c->x1 = c->kind == 4 ? predicate.value("upper").toDouble()
                             : predicate.value("half_width_degrees").toDouble() / 180.0;
        c->feather = predicate.value("softness").toDouble();
        c->leaf_invert = inverse;
        c->condition_expression.clear();
    }
    parameterEdited(QStringLiteral("local_mask/conditions"), before);
    emit gradeNodesChanged();
}
