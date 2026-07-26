#include "edit_controller.hpp"

#include <QJsonDocument>
#include <QSet>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <optional>
#include <utility>

namespace {

constexpr auto NODE_MASK_ASSETS_SETTINGS_KEY = "precision/node_mask_assets_v1";
constexpr int MAX_NODE_MASK_ASSET_COUNT = 128;
constexpr int MAX_NODE_MASK_ASSET_NAME_LENGTH = 96;

[[nodiscard]] std::optional<QVariantMap> normalized_node_mask_asset(
    const QVariantMap& source
) {
    const QString id = source.value(QStringLiteral("id")).toString();
    const QString name = source.value(QStringLiteral("name")).toString().trimmed();
    if (QUuid(id).isNull() || name.isEmpty()
        || name.size() > MAX_NODE_MASK_ASSET_NAME_LENGTH) {
        return std::nullopt;
    }

    bool kind_ok = false;
    const int kind = source.value(QStringLiteral("kind")).toInt(&kind_ok);
    if (!kind_ok || kind < 1 || kind > 3) {
        return std::nullopt;
    }

    const auto unit_value = [&source](const QString& key) -> std::optional<double> {
        bool converted = false;
        const double value = source.value(key).toDouble(&converted);
        if (!converted || !std::isfinite(value) || value < 0.0 || value > 1.0) {
            return std::nullopt;
        }
        return value;
    };
    const auto x0 = unit_value(QStringLiteral("x0"));
    const auto y0 = unit_value(QStringLiteral("y0"));
    const auto x1 = unit_value(QStringLiteral("x1"));
    const auto y1 = unit_value(QStringLiteral("y1"));
    const auto radius_x = unit_value(QStringLiteral("radiusX"));
    const auto radius_y = unit_value(QStringLiteral("radiusY"));
    const auto feather = unit_value(QStringLiteral("feather"));
    if (!x0 || !y0 || !x1 || !y1 || !radius_x || !radius_y || !feather) {
        return std::nullopt;
    }

    QVariantList brush_points;
    const QVariantList candidate_points =
        source.value(QStringLiteral("brushPoints")).toList();
    constexpr int maximum_values = 3 * 4'096;
    if (candidate_points.size() > maximum_values || candidate_points.size() % 3 != 0) {
        return std::nullopt;
    }
    brush_points.reserve(candidate_points.size());
    for (qsizetype index = 0; index < candidate_points.size(); ++index) {
        bool converted = false;
        const double value = candidate_points.at(index).toDouble(&converted);
        const bool is_stroke_marker = index % 3 == 2;
        if (!converted || !std::isfinite(value)
            || (!is_stroke_marker && (value < 0.0 || value > 1.0))
            || (is_stroke_marker && value != 0.0 && value != 1.0)) {
            return std::nullopt;
        }
        brush_points.push_back(value);
    }

    if ((kind == 1 && std::hypot(*x1 - *x0, *y1 - *y0) <= std::numeric_limits<double>::epsilon())
        || (kind == 2 && (*radius_x <= 0.0 || *radius_y <= 0.0))
        || (kind == 3 && *radius_x <= 0.0)) {
        return std::nullopt;
    }

    return QVariantMap{
        {QStringLiteral("id"), id},
        {QStringLiteral("name"), name},
        {QStringLiteral("kind"), kind},
        {QStringLiteral("x0"), *x0},
        {QStringLiteral("y0"), *y0},
        {QStringLiteral("x1"), *x1},
        {QStringLiteral("y1"), *y1},
        {QStringLiteral("radiusX"), *radius_x},
        {QStringLiteral("radiusY"), *radius_y},
        {QStringLiteral("feather"), *feather},
        {QStringLiteral("inverted"), source.value(QStringLiteral("inverted")).toBool()},
        {QStringLiteral("brushPoints"), brush_points},
    };
}

[[nodiscard]] QVariantMap node_mask_asset_from_node(
    const QString& id,
    const QString& name,
    const BackendGradeNode& node
) {
    QVariantList brush_points;
    brush_points.reserve(node.local_mask_brush_points.size());
    for (const double value : node.local_mask_brush_points) {
        brush_points.push_back(value);
    }
    return {
        {QStringLiteral("id"), id},
        {QStringLiteral("name"), name},
        {QStringLiteral("kind"), static_cast<int>(node.local_mask_kind)},
        {QStringLiteral("x0"), node.local_mask_x0},
        {QStringLiteral("y0"), node.local_mask_y0},
        {QStringLiteral("x1"), node.local_mask_x1},
        {QStringLiteral("y1"), node.local_mask_y1},
        {QStringLiteral("radiusX"), node.local_mask_radius_x},
        {QStringLiteral("radiusY"), node.local_mask_radius_y},
        {QStringLiteral("feather"), node.local_mask_feather},
        {QStringLiteral("inverted"), node.local_mask_invert},
        {QStringLiteral("brushPoints"), brush_points},
    };
}

[[nodiscard]] LocalizedUiMessage local_mask_message(
    const char* const source,
    const std::initializer_list<LocalizedUiArgument> arguments = {}
) {
    return {"EditController", source, arguments};
}

} // namespace

QVariantMap EditController::selectedLocalMask() const {
    const auto* const grade_node = selectedGradeNode();
    if (grade_node == nullptr) {
        return {{QStringLiteral("kind"), 0}};
    }
    QVariantList brush_points;
    brush_points.reserve(grade_node->local_mask_brush_points.size() / 3);
    for (qsizetype offset = 0;
         offset + 2 < grade_node->local_mask_brush_points.size();
         offset += 3) {
        brush_points.push_back(QVariantMap{
            {QStringLiteral("x"), grade_node->local_mask_brush_points.at(offset)},
            {QStringLiteral("y"), grade_node->local_mask_brush_points.at(offset + 1)},
            {
                QStringLiteral("beginsStroke"),
                grade_node->local_mask_brush_points.at(offset + 2) == 1.0
            },
        });
    }
    return {
        {QStringLiteral("kind"), static_cast<int>(grade_node->local_mask_kind)},
        {QStringLiteral("x0"), grade_node->local_mask_x0},
        {QStringLiteral("y0"), grade_node->local_mask_y0},
        {QStringLiteral("x1"), grade_node->local_mask_x1},
        {QStringLiteral("y1"), grade_node->local_mask_y1},
        {QStringLiteral("radiusX"), grade_node->local_mask_radius_x},
        {QStringLiteral("radiusY"), grade_node->local_mask_radius_y},
        {QStringLiteral("feather"), grade_node->local_mask_feather},
        {QStringLiteral("inverted"), grade_node->local_mask_invert},
        {QStringLiteral("brushPoints"), brush_points},
    };
}

bool EditController::hasCopiedNodeMask() const noexcept {
    return node_mask_clipboard_.has_value();
}

QVariantList EditController::nodeMaskAssets() const {
    return node_mask_assets_;
}

void EditController::loadNodeMaskAssets() {
    const QByteArray stored = node_mask_asset_settings_->value(
        QString::fromLatin1(NODE_MASK_ASSETS_SETTINGS_KEY)
    ).toByteArray();
    const QJsonDocument parsed = QJsonDocument::fromJson(stored);
    if (!parsed.isArray()) {
        return;
    }

    QSet<QString> ids;
    QSet<QString> names;
    const QVariantList stored_assets = parsed.toVariant().toList();
    for (const QVariant& value : stored_assets) {
        const auto asset = normalized_node_mask_asset(value.toMap());
        if (!asset.has_value() || node_mask_assets_.size() >= MAX_NODE_MASK_ASSET_COUNT) {
            continue;
        }
        const QString id = asset->value(QStringLiteral("id")).toString();
        const QString name = asset->value(QStringLiteral("name")).toString();
        const QString name_key = name.toCaseFolded();
        if (ids.contains(id) || names.contains(name_key)) {
            continue;
        }
        ids.insert(id);
        names.insert(name_key);
        node_mask_assets_.push_back(*asset);
    }
    std::sort(
        node_mask_assets_.begin(),
        node_mask_assets_.end(),
        [](const QVariant& left, const QVariant& right) {
            return left.toMap().value(QStringLiteral("name")).toString().compare(
                right.toMap().value(QStringLiteral("name")).toString(),
                Qt::CaseInsensitive
            ) < 0;
        }
    );
}

void EditController::persistNodeMaskAssets() {
    node_mask_asset_settings_->setValue(
        QString::fromLatin1(NODE_MASK_ASSETS_SETTINGS_KEY),
        QJsonDocument::fromVariant(node_mask_assets_).toJson(QJsonDocument::Compact)
    );
    node_mask_asset_settings_->sync();
}

void EditController::setSelectedLocalMask(const int kind) {
    auto* const grade_node = selected_grade_node_index_ < 0
        ? nullptr : &grade_stack_.grade_nodes[selected_grade_node_index_];
    if (!active_ || interactionLocked() || grade_node == nullptr || !grade_node->enabled
        || kind < 0 || kind > 3 || grade_node->local_mask_kind == kind) {
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    grade_node->local_mask_kind = static_cast<std::uint8_t>(kind);
    grade_node->local_mask_invert = false;
    grade_node->local_mask_brush_points.clear();
    if (kind == 0) {
        grade_node->local_mask_x0 = 0.0;
        grade_node->local_mask_y0 = 0.0;
        grade_node->local_mask_x1 = 0.0;
        grade_node->local_mask_y1 = 0.0;
        grade_node->local_mask_radius_x = 0.0;
        grade_node->local_mask_radius_y = 0.0;
        grade_node->local_mask_feather = 0.0;
    } else if (kind == 1) {
        grade_node->local_mask_x0 = 0.25;
        grade_node->local_mask_y0 = 0.5;
        grade_node->local_mask_x1 = 0.75;
        grade_node->local_mask_y1 = 0.5;
        grade_node->local_mask_radius_x = 0.0;
        grade_node->local_mask_radius_y = 0.0;
        grade_node->local_mask_feather = 0.0;
    } else if (kind == 2) {
        grade_node->local_mask_x0 = 0.5;
        grade_node->local_mask_y0 = 0.5;
        grade_node->local_mask_x1 = 0.0;
        grade_node->local_mask_y1 = 0.0;
        grade_node->local_mask_radius_x = 0.28;
        grade_node->local_mask_radius_y = 0.28;
        grade_node->local_mask_feather = 0.35;
    } else {
        grade_node->local_mask_x0 = 0.0;
        grade_node->local_mask_y0 = 0.0;
        grade_node->local_mask_x1 = 0.0;
        grade_node->local_mask_y1 = 0.0;
        grade_node->local_mask_radius_x = 0.035;
        grade_node->local_mask_radius_y = 0.0;
        grade_node->local_mask_feather = 0.6;
    }
    parameterEdited(QStringLiteral("local_mask/kind"), before);
    emit gradeNodesChanged();
    setStatusMessage(local_mask_message(QT_TRANSLATE_NOOP(
        "EditController",
        kind == 0 ? "Removed local mask"
                  : kind == 1 ? "Added linear gradient mask"
                  : kind == 2 ? "Added radial gradient mask"
                              : "Added brush mask"
    )));
}

void EditController::copySelectedLocalMask() {
    const auto* const grade_node = selectedGradeNode();
    if (!active_ || interactionLocked() || grade_node == nullptr
        || grade_node->local_mask_kind == 0U) {
        setStatusMessage(local_mask_message(QT_TRANSLATE_NOOP(
            "EditController", "Select a node mask to copy"
        )));
        return;
    }
    finishActiveGesture();
    node_mask_clipboard_ = NodeMaskClipboard{
        .kind = grade_node->local_mask_kind,
        .x0 = grade_node->local_mask_x0,
        .y0 = grade_node->local_mask_y0,
        .x1 = grade_node->local_mask_x1,
        .y1 = grade_node->local_mask_y1,
        .radius_x = grade_node->local_mask_radius_x,
        .radius_y = grade_node->local_mask_radius_y,
        .feather = grade_node->local_mask_feather,
        .inverted = grade_node->local_mask_invert,
        .brush_points = grade_node->local_mask_brush_points,
    };
    emit nodeMaskClipboardChanged();
    setStatusMessage(local_mask_message(QT_TRANSLATE_NOOP(
        "EditController", "Copied node mask"
    )));
}

void EditController::pasteSelectedLocalMask() {
    if (!node_mask_clipboard_.has_value()) {
        setStatusMessage(local_mask_message(QT_TRANSLATE_NOOP(
            "EditController", "Copy a node mask before pasting"
        )));
        return;
    }
    auto* const grade_node = selected_grade_node_index_ < 0
        ? nullptr : &grade_stack_.grade_nodes[selected_grade_node_index_];
    if (!active_ || interactionLocked() || grade_node == nullptr || !grade_node->enabled) {
        setStatusMessage(local_mask_message(QT_TRANSLATE_NOOP(
            "EditController", "Select an enabled Grade Node to paste the mask"
        )));
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    BackendGradeNode candidate = *grade_node;
    const NodeMaskClipboard& source = *node_mask_clipboard_;
    candidate.local_mask_kind = source.kind;
    candidate.local_mask_x0 = source.x0;
    candidate.local_mask_y0 = source.y0;
    candidate.local_mask_x1 = source.x1;
    candidate.local_mask_y1 = source.y1;
    candidate.local_mask_radius_x = source.radius_x;
    candidate.local_mask_radius_y = source.radius_y;
    candidate.local_mask_feather = source.feather;
    candidate.local_mask_invert = source.inverted;
    candidate.local_mask_brush_points = source.brush_points;
    if (candidate == *grade_node) {
        setStatusMessage(local_mask_message(QT_TRANSLATE_NOOP(
            "EditController", "Selected Grade Node already uses the copied mask"
        )));
        return;
    }
    *grade_node = std::move(candidate);
    parameterEdited(QStringLiteral("local_mask/paste"), before);
    emit gradeNodesChanged();
    setStatusMessage(local_mask_message(QT_TRANSLATE_NOOP(
        "EditController", "Pasted node mask"
    )));
}

void EditController::saveSelectedLocalMaskAsset(const QString& name) {
    const QString normalized_name = name.trimmed();
    const auto* const grade_node = selectedGradeNode();
    if (!active_ || interactionLocked() || grade_node == nullptr
        || grade_node->local_mask_kind == 0U) {
        setStatusMessage(local_mask_message(QT_TRANSLATE_NOOP(
            "EditController", "Select a node mask to save as an asset"
        )));
        return;
    }
    if (normalized_name.isEmpty()) {
        setStatusMessage(local_mask_message(QT_TRANSLATE_NOOP(
            "EditController", "Enter a mask asset name"
        )));
        return;
    }
    if (normalized_name.size() > MAX_NODE_MASK_ASSET_NAME_LENGTH) {
        setStatusMessage(local_mask_message(QT_TRANSLATE_NOOP(
            "EditController", "Mask asset names can contain at most %1 characters"
        ), {MAX_NODE_MASK_ASSET_NAME_LENGTH}));
        return;
    }

    finishActiveGesture();
    QString id;
    qsizetype matching_index = -1;
    for (qsizetype index = 0; index < node_mask_assets_.size(); ++index) {
        const QVariantMap existing = node_mask_assets_.at(index).toMap();
        if (existing.value(QStringLiteral("name")).toString().compare(
                normalized_name,
                Qt::CaseInsensitive
            ) == 0) {
            id = existing.value(QStringLiteral("id")).toString();
            matching_index = index;
            break;
        }
    }
    if (matching_index < 0 && node_mask_assets_.size() >= MAX_NODE_MASK_ASSET_COUNT) {
        setStatusMessage(local_mask_message(QT_TRANSLATE_NOOP(
            "EditController", "Mask asset library is full"
        )));
        return;
    }
    if (id.isEmpty()) {
        id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }

    const auto asset = normalized_node_mask_asset(
        node_mask_asset_from_node(id, normalized_name, *grade_node)
    );
    if (!asset.has_value()) {
        setStatusMessage(local_mask_message(QT_TRANSLATE_NOOP(
            "EditController", "Current node mask cannot be saved as an asset"
        )));
        return;
    }
    if (matching_index >= 0) {
        node_mask_assets_[matching_index] = *asset;
    } else {
        node_mask_assets_.push_back(*asset);
    }
    std::sort(
        node_mask_assets_.begin(),
        node_mask_assets_.end(),
        [](const QVariant& left, const QVariant& right) {
            return left.toMap().value(QStringLiteral("name")).toString().compare(
                right.toMap().value(QStringLiteral("name")).toString(),
                Qt::CaseInsensitive
            ) < 0;
        }
    );
    persistNodeMaskAssets();
    emit nodeMaskAssetsChanged();
    setStatusMessage(local_mask_message(QT_TRANSLATE_NOOP(
        "EditController", "Saved node mask asset · %1"
    ), {normalized_name}));
}

void EditController::applySelectedLocalMaskAsset(const QString& asset_id) {
    auto* const grade_node = selected_grade_node_index_ < 0
        ? nullptr : &grade_stack_.grade_nodes[selected_grade_node_index_];
    if (!active_ || interactionLocked() || grade_node == nullptr || !grade_node->enabled) {
        setStatusMessage(local_mask_message(QT_TRANSLATE_NOOP(
            "EditController", "Select an enabled Grade Node to apply a mask asset"
        )));
        return;
    }
    const auto found = std::find_if(
        node_mask_assets_.cbegin(),
        node_mask_assets_.cend(),
        [&asset_id](const QVariant& value) {
            return value.toMap().value(QStringLiteral("id")).toString() == asset_id;
        }
    );
    if (found == node_mask_assets_.cend()) {
        setStatusMessage(local_mask_message(QT_TRANSLATE_NOOP(
            "EditController", "Choose a saved mask asset to apply"
        )));
        return;
    }
    const QVariantMap asset = found->toMap();
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    BackendGradeNode candidate = *grade_node;
    candidate.local_mask_kind = static_cast<std::uint8_t>(
        asset.value(QStringLiteral("kind")).toInt()
    );
    candidate.local_mask_x0 = asset.value(QStringLiteral("x0")).toDouble();
    candidate.local_mask_y0 = asset.value(QStringLiteral("y0")).toDouble();
    candidate.local_mask_x1 = asset.value(QStringLiteral("x1")).toDouble();
    candidate.local_mask_y1 = asset.value(QStringLiteral("y1")).toDouble();
    candidate.local_mask_radius_x = asset.value(QStringLiteral("radiusX")).toDouble();
    candidate.local_mask_radius_y = asset.value(QStringLiteral("radiusY")).toDouble();
    candidate.local_mask_feather = asset.value(QStringLiteral("feather")).toDouble();
    candidate.local_mask_invert = asset.value(QStringLiteral("inverted")).toBool();
    candidate.local_mask_brush_points.clear();
    const QVariantList brush_points = asset.value(QStringLiteral("brushPoints")).toList();
    candidate.local_mask_brush_points.reserve(brush_points.size());
    for (const QVariant& value : brush_points) {
        candidate.local_mask_brush_points.push_back(value.toDouble());
    }
    if (candidate == *grade_node) {
        setStatusMessage(local_mask_message(QT_TRANSLATE_NOOP(
            "EditController", "Selected Grade Node already uses this mask asset"
        )));
        return;
    }
    *grade_node = std::move(candidate);
    parameterEdited(QStringLiteral("local_mask/apply_asset"), before);
    emit gradeNodesChanged();
    setStatusMessage(local_mask_message(QT_TRANSLATE_NOOP(
        "EditController", "Applied node mask asset · %1"
    ), {asset.value(QStringLiteral("name")).toString()}));
}

void EditController::removeLocalMaskAsset(const QString& asset_id) {
    const auto found = std::find_if(
        node_mask_assets_.cbegin(),
        node_mask_assets_.cend(),
        [&asset_id](const QVariant& value) {
            return value.toMap().value(QStringLiteral("id")).toString() == asset_id;
        }
    );
    if (found == node_mask_assets_.cend()) {
        return;
    }
    const QString name = found->toMap().value(QStringLiteral("name")).toString();
    node_mask_assets_.erase(found);
    persistNodeMaskAssets();
    emit nodeMaskAssetsChanged();
    setStatusMessage(local_mask_message(QT_TRANSLATE_NOOP(
        "EditController", "Removed node mask asset · %1"
    ), {name}));
}

void EditController::setSelectedLocalMaskValue(
    const QString& key,
    const double value
) {
    auto* const grade_node = selected_grade_node_index_ < 0
        ? nullptr : &grade_stack_.grade_nodes[selected_grade_node_index_];
    if (grade_node == nullptr || grade_node->local_mask_kind == 0U
        || !acceptParameter(
            value,
            0.0,
            1.0,
            QT_TRANSLATE_NOOP("EditController", "Local mask")
        )) {
        return;
    }

    double* target = nullptr;
    if (key == QStringLiteral("x0")) target = &grade_node->local_mask_x0;
    else if (key == QStringLiteral("y0")) target = &grade_node->local_mask_y0;
    else if (key == QStringLiteral("x1")) target = &grade_node->local_mask_x1;
    else if (key == QStringLiteral("y1")) target = &grade_node->local_mask_y1;
    else if (key == QStringLiteral("radiusX")) target = &grade_node->local_mask_radius_x;
    else if (key == QStringLiteral("radiusY")) target = &grade_node->local_mask_radius_y;
    else if (key == QStringLiteral("feather")) target = &grade_node->local_mask_feather;
    if (target == nullptr || *target == value) {
        return;
    }

    BackendGradeNode candidate = *grade_node;
    if (key == QStringLiteral("x0")) candidate.local_mask_x0 = value;
    else if (key == QStringLiteral("y0")) candidate.local_mask_y0 = value;
    else if (key == QStringLiteral("x1")) candidate.local_mask_x1 = value;
    else if (key == QStringLiteral("y1")) candidate.local_mask_y1 = value;
    else if (key == QStringLiteral("radiusX")) candidate.local_mask_radius_x = value;
    else if (key == QStringLiteral("radiusY")) candidate.local_mask_radius_y = value;
    else candidate.local_mask_feather = value;
    if (candidate.local_mask_kind == 1U
        && std::hypot(
            candidate.local_mask_x1 - candidate.local_mask_x0,
            candidate.local_mask_y1 - candidate.local_mask_y0
        ) < 0.01) {
        setStatusMessage(local_mask_message(QT_TRANSLATE_NOOP(
            "EditController", "A gradient mask needs two distinct points"
        )));
        return;
    }
    if (candidate.local_mask_kind == 2U
        && (candidate.local_mask_radius_x < 0.01
            || candidate.local_mask_radius_y < 0.01)) {
        setStatusMessage(local_mask_message(QT_TRANSLATE_NOOP(
            "EditController", "A radial mask needs a non-zero radius"
        )));
        return;
    }
    if (candidate.local_mask_kind == 3U
        && candidate.local_mask_radius_x < 0.005) {
        setStatusMessage(local_mask_message(QT_TRANSLATE_NOOP(
            "EditController", "A brush mask needs a non-zero size"
        )));
        return;
    }

    const BackendGradeStack before = grade_stack_;
    *grade_node = std::move(candidate);
    parameterEdited(QStringLiteral("local_mask/%1").arg(key), before);
    emit gradeNodesChanged();
}

void EditController::setSelectedLocalMaskPoint(
    const QString& point,
    const double normalized_x,
    const double normalized_y
) {
    auto* const grade_node = selected_grade_node_index_ < 0
        ? nullptr : &grade_stack_.grade_nodes[selected_grade_node_index_];
    if (grade_node == nullptr || grade_node->local_mask_kind == 0U
        || !acceptParameter(
            normalized_x,
            0.0,
            1.0,
            QT_TRANSLATE_NOOP("EditController", "Local mask")
        )
        || !acceptParameter(
            normalized_y,
            0.0,
            1.0,
            QT_TRANSLATE_NOOP("EditController", "Local mask")
        )) {
        return;
    }

    BackendGradeNode candidate = *grade_node;
    if (point == QStringLiteral("start") && candidate.local_mask_kind == 1U) {
        candidate.local_mask_x0 = normalized_x;
        candidate.local_mask_y0 = normalized_y;
    } else if (point == QStringLiteral("end")
               && candidate.local_mask_kind == 1U) {
        candidate.local_mask_x1 = normalized_x;
        candidate.local_mask_y1 = normalized_y;
    } else if (point == QStringLiteral("center")
               && candidate.local_mask_kind == 2U) {
        candidate.local_mask_x0 = normalized_x;
        candidate.local_mask_y0 = normalized_y;
    } else {
        return;
    }

    if (candidate == *grade_node) {
        return;
    }
    if (candidate.local_mask_kind == 1U
        && std::hypot(
            candidate.local_mask_x1 - candidate.local_mask_x0,
            candidate.local_mask_y1 - candidate.local_mask_y0
        ) < 0.01) {
        setStatusMessage(local_mask_message(QT_TRANSLATE_NOOP(
            "EditController", "A gradient mask needs two distinct points"
        )));
        return;
    }

    const BackendGradeStack before = grade_stack_;
    *grade_node = std::move(candidate);
    parameterEdited(QStringLiteral("local_mask/%1").arg(point), before);
    emit gradeNodesChanged();
}

void EditController::setSelectedLocalMaskInverted(const bool inverted) {
    auto* const grade_node = selected_grade_node_index_ < 0
        ? nullptr : &grade_stack_.grade_nodes[selected_grade_node_index_];
    if (!active_ || interactionLocked() || grade_node == nullptr || !grade_node->enabled
        || grade_node->local_mask_kind == 0U
        || grade_node->local_mask_invert == inverted) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    grade_node->local_mask_invert = inverted;
    parameterEdited(QStringLiteral("local_mask/invert"), before);
    emit gradeNodesChanged();
}

void EditController::appendSelectedLocalMaskBrushPoint(
    const double normalized_x,
    const double normalized_y,
    const bool begins_stroke
) {
    constexpr qsizetype maximum_brush_points = 4'096;
    constexpr double minimum_point_distance = 0.0015;
    auto* const grade_node = selected_grade_node_index_ < 0
        ? nullptr : &grade_stack_.grade_nodes[selected_grade_node_index_];
    if (grade_node == nullptr || grade_node->local_mask_kind != 3U
        || !acceptParameter(
            normalized_x,
            0.0,
            1.0,
            QT_TRANSLATE_NOOP("EditController", "Brush mask")
        )
        || !acceptParameter(
            normalized_y,
            0.0,
            1.0,
            QT_TRANSLATE_NOOP("EditController", "Brush mask")
        )) {
        return;
    }
    const qsizetype point_count = grade_node->local_mask_brush_points.size() / 3;
    if (point_count >= maximum_brush_points) {
        setStatusMessage(local_mask_message(QT_TRANSLATE_NOOP(
            "EditController", "This brush mask has reached its point limit"
        )));
        return;
    }
    if (!begins_stroke && point_count > 0) {
        const qsizetype previous = grade_node->local_mask_brush_points.size() - 3;
        if (std::hypot(
                normalized_x - grade_node->local_mask_brush_points.at(previous),
                normalized_y - grade_node->local_mask_brush_points.at(previous + 1)
            ) < minimum_point_distance) {
            return;
        }
    }
    const BackendGradeStack before = grade_stack_;
    grade_node->local_mask_brush_points.push_back(normalized_x);
    grade_node->local_mask_brush_points.push_back(normalized_y);
    grade_node->local_mask_brush_points.push_back(begins_stroke ? 1.0 : 0.0);
    parameterEdited(QStringLiteral("local_mask/brush"), before);
    emit gradeNodesChanged();
}

void EditController::clearSelectedLocalMaskBrush() {
    auto* const grade_node = selected_grade_node_index_ < 0
        ? nullptr : &grade_stack_.grade_nodes[selected_grade_node_index_];
    if (!active_ || interactionLocked() || grade_node == nullptr
        || grade_node->local_mask_kind != 3U
        || grade_node->local_mask_brush_points.isEmpty()) {
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    grade_node->local_mask_brush_points.clear();
    parameterEdited(QStringLiteral("local_mask/brush/clear"), before);
    emit gradeNodesChanged();
}
