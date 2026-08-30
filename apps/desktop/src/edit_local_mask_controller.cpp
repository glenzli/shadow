#include "edit_controller.hpp"
#include "edit_ai_mask_controller.hpp"
#include "edit_stroke_input.hpp"

#include <QLatin1StringView>

#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <string_view>
#include <utility>

namespace {

[[nodiscard]] LocalizedUiMessage local_mask_message(
    const char* const source,
    const std::initializer_list<LocalizedUiArgument> arguments = {}
) {
    return {"EditController", source, arguments};
}

struct LocalMaskParameterDescriptor final {
    std::string_view key;
    double BackendGradeNode::* field = nullptr;
    std::uint8_t kind_mask = 0U;
    double minimum = 0.0;
    double maximum = 1.0;
};

[[nodiscard]] constexpr std::uint8_t mask_kind_bit(const std::uint8_t kind) noexcept {
    return static_cast<std::uint8_t>(1U << kind);
}

constexpr std::array local_mask_parameters{
    LocalMaskParameterDescriptor{
        .key = "x0",
        .field = &BackendGradeNode::local_mask_x0,
        .kind_mask = mask_kind_bit(1U) | mask_kind_bit(2U),
    },
    LocalMaskParameterDescriptor{
        .key = "y0",
        .field = &BackendGradeNode::local_mask_y0,
        .kind_mask = mask_kind_bit(1U) | mask_kind_bit(2U),
    },
    LocalMaskParameterDescriptor{
        .key = "x1",
        .field = &BackendGradeNode::local_mask_x1,
        .kind_mask = mask_kind_bit(1U),
    },
    LocalMaskParameterDescriptor{
        .key = "y1",
        .field = &BackendGradeNode::local_mask_y1,
        .kind_mask = mask_kind_bit(1U),
    },
    LocalMaskParameterDescriptor{
        .key = "radiusX",
        .field = &BackendGradeNode::local_mask_radius_x,
        .kind_mask = mask_kind_bit(2U) | mask_kind_bit(3U),
        .minimum = 0.005,
    },
    LocalMaskParameterDescriptor{
        .key = "radiusY",
        .field = &BackendGradeNode::local_mask_radius_y,
        .kind_mask = mask_kind_bit(2U),
        .minimum = 0.01,
    },
    LocalMaskParameterDescriptor{
        .key = "feather",
        .field = &BackendGradeNode::local_mask_feather,
        .kind_mask = mask_kind_bit(2U) | mask_kind_bit(3U) | mask_kind_bit(6U),
    },
    LocalMaskParameterDescriptor{
        .key = "x0",
        .field = &BackendGradeNode::local_mask_x0,
        .kind_mask = mask_kind_bit(6U),
        .minimum = -1.0,
    },
    LocalMaskParameterDescriptor{
        .key = "lower",
        .field = &BackendGradeNode::local_mask_x0,
        .kind_mask = mask_kind_bit(4U),
    },
    LocalMaskParameterDescriptor{
        .key = "upper",
        .field = &BackendGradeNode::local_mask_x1,
        .kind_mask = mask_kind_bit(4U),
    },
    LocalMaskParameterDescriptor{
        .key = "softness",
        .field = &BackendGradeNode::local_mask_feather,
        .kind_mask = mask_kind_bit(4U) | mask_kind_bit(5U),
    },
    LocalMaskParameterDescriptor{
        .key = "centerHue",
        .field = &BackendGradeNode::local_mask_x0,
        .kind_mask = mask_kind_bit(5U),
        .maximum = 359.0 / 360.0,
    },
    LocalMaskParameterDescriptor{
        .key = "width",
        .field = &BackendGradeNode::local_mask_x1,
        .kind_mask = mask_kind_bit(5U),
        .minimum = 1.0 / 180.0,
    },
};

[[nodiscard]] const LocalMaskParameterDescriptor*
local_mask_parameter(const QString& key, const std::uint8_t kind) noexcept {
    const std::uint8_t kind_bit = mask_kind_bit(kind);
    for (const auto& descriptor : local_mask_parameters) {
        if ((descriptor.kind_mask & kind_bit) != 0U && key == QLatin1StringView(descriptor.key)) {
            return &descriptor;
        }
    }
    return nullptr;
}

} // namespace

QVariantMap EditController::selectedLocalMask() const {
    const auto* const grade_node = selectedGradeNode();
    if (grade_node == nullptr) {
        return {{QStringLiteral("kind"), 0}};
    }
    QVariantList brush_points;
    brush_points.reserve(grade_node->local_mask_brush_points.size() / 3);
    for (qsizetype offset = 0; offset + 2 < grade_node->local_mask_brush_points.size();
         offset += 3) {
        brush_points.push_back(
            QVariantMap{
                {QStringLiteral("x"), grade_node->local_mask_brush_points.at(offset)},
                {QStringLiteral("y"), grade_node->local_mask_brush_points.at(offset + 1)},
                {QStringLiteral("beginsStroke"),
                 grade_node->local_mask_brush_points.at(offset + 2) == 1.0},
            }
        );
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
        {QStringLiteral("lower"), grade_node->local_mask_x0},
        {QStringLiteral("upper"), grade_node->local_mask_x1},
        {QStringLiteral("centerHue"), grade_node->local_mask_x0},
        {QStringLiteral("width"), grade_node->local_mask_x1},
        {QStringLiteral("softness"), grade_node->local_mask_feather},
        {QStringLiteral("inverted"), grade_node->local_mask_invert},
        {QStringLiteral("brushPoints"), brush_points},
        {QStringLiteral("semanticQuery"), grade_node->local_mask_semantic_query},
        {QStringLiteral("semanticMaximumRegions"),
         static_cast<int>(grade_node->local_mask_semantic_maximum_regions)},
        {QStringLiteral("semanticScoreThresholdPercent"),
         static_cast<int>(grade_node->local_mask_semantic_score_threshold_percent)},
    };
}

bool EditController::hasCopiedNodeMask() const noexcept {
    return node_mask_clipboard_.has_value();
}

void EditController::setSelectedLocalMask(const int kind) {
    auto* const grade_node = selected_grade_node_index_ < 0
                                 ? nullptr
                                 : &grade_stack_.grade_nodes[selected_grade_node_index_];
    if (!active_ || interactionLocked() || grade_node == nullptr || !grade_node->enabled || kind < 0
        || kind > 5 || grade_node->local_mask_kind == kind) {
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    initializeLocalMask(*grade_node, kind);
    parameterEdited(QStringLiteral("local_mask/kind"), before);
    emit gradeNodesChanged();
    setStatusMessage(local_mask_message(QT_TRANSLATE_NOOP(
        "EditController",
        kind == 0   ? "Removed local mask"
        : kind == 1 ? "Added linear gradient mask"
        : kind == 2 ? "Added radial gradient mask"
        : kind == 3 ? "Added brush mask"
        : kind == 4 ? "Added luminance range mask"
                    : "Added color range mask"
    )));
}

void EditController::copySelectedLocalMask() {
    const auto* const grade_node = selectedGradeNode();
    if (!active_ || interactionLocked() || grade_node == nullptr
        || grade_node->local_mask_kind == 0U) {
        setStatusMessage(
            local_mask_message(QT_TRANSLATE_NOOP("EditController", "Select a node mask to copy"))
        );
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
        .semantic_query = grade_node->local_mask_semantic_query,
        .semantic_maximum_regions = grade_node->local_mask_semantic_maximum_regions,
        .semantic_score_threshold_percent =
            grade_node->local_mask_semantic_score_threshold_percent,
    };
    emit nodeMaskClipboardChanged();
    setStatusMessage(local_mask_message(QT_TRANSLATE_NOOP("EditController", "Copied node mask")));
}

void EditController::pasteSelectedLocalMask() {
    if (!node_mask_clipboard_.has_value()) {
        setStatusMessage(local_mask_message(
            QT_TRANSLATE_NOOP("EditController", "Copy a node mask before pasting")
        ));
        return;
    }
    auto* const grade_node = selected_grade_node_index_ < 0
                                 ? nullptr
                                 : &grade_stack_.grade_nodes[selected_grade_node_index_];
    if (!active_ || interactionLocked() || grade_node == nullptr || !grade_node->enabled) {
        setStatusMessage(local_mask_message(
            QT_TRANSLATE_NOOP("EditController", "Select an enabled Grade Node to paste the mask")
        ));
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    BackendGradeNode candidate = *grade_node;
    const NodeMaskClipboard& source = *node_mask_clipboard_;
    if (source.kind == 6U) {
        if (source.semantic_query.isEmpty()) {
            setStatusMessage(local_mask_message(QT_TRANSLATE_NOOP(
                "EditController",
                "This AI mask has no reusable semantic instruction"
            )));
            return;
        }
        if (ai_mask_controller_ != nullptr
            && ai_mask_controller_->beginSemantic(
                source.semantic_query,
                source.semantic_maximum_regions,
                source.semantic_score_threshold_percent,
                true
            )) {
            setStatusMessage(local_mask_message(QT_TRANSLATE_NOOP(
                "EditController",
                "Re-evaluating copied Semantic Mask on this photo…"
            )));
        }
        return;
    }
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
    candidate.local_mask_semantic_query.clear();
    candidate.local_mask_semantic_maximum_regions = 0U;
    candidate.local_mask_semantic_score_threshold_percent = 0U;
    if (candidate == *grade_node) {
        setStatusMessage(local_mask_message(
            QT_TRANSLATE_NOOP("EditController", "Selected Grade Node already uses the copied mask")
        ));
        return;
    }
    *grade_node = std::move(candidate);
    parameterEdited(QStringLiteral("local_mask/paste"), before);
    emit gradeNodesChanged();
    setStatusMessage(local_mask_message(QT_TRANSLATE_NOOP("EditController", "Pasted node mask")));
}

void EditController::setSelectedLocalMaskValue(const QString& key, const double value) {
    auto* const grade_node = selected_grade_node_index_ < 0
                                 ? nullptr
                                 : &grade_stack_.grade_nodes[selected_grade_node_index_];
    if (grade_node == nullptr || grade_node->local_mask_kind == 0U) {
        return;
    }
    const auto* const descriptor = local_mask_parameter(key, grade_node->local_mask_kind);
    if (descriptor == nullptr
        || !acceptParameter(
            value,
            descriptor->minimum,
            descriptor->maximum,
            QT_TRANSLATE_NOOP("EditController", "Local mask")
        )
        || grade_node->*(descriptor->field) == value) {
        return;
    }

    BackendGradeNode candidate = *grade_node;
    candidate.*(descriptor->field) = value;
    if (candidate.local_mask_kind == 1U
        && std::hypot(
               candidate.local_mask_x1 - candidate.local_mask_x0,
               candidate.local_mask_y1 - candidate.local_mask_y0
           ) < 0.01) {
        setStatusMessage(local_mask_message(
            QT_TRANSLATE_NOOP("EditController", "A gradient mask needs two distinct points")
        ));
        return;
    }
    if (candidate.local_mask_kind == 2U
        && (candidate.local_mask_radius_x < 0.01 || candidate.local_mask_radius_y < 0.01)) {
        setStatusMessage(local_mask_message(
            QT_TRANSLATE_NOOP("EditController", "A radial mask needs a non-zero radius")
        ));
        return;
    }
    if (candidate.local_mask_kind == 3U && candidate.local_mask_radius_x < 0.005) {
        setStatusMessage(local_mask_message(
            QT_TRANSLATE_NOOP("EditController", "A brush mask needs a non-zero size")
        ));
        return;
    }
    if (candidate.local_mask_kind == 4U && candidate.local_mask_x0 > candidate.local_mask_x1) {
        setStatusMessage(local_mask_message(QT_TRANSLATE_NOOP(
            "EditController",
            "The lower lightness limit cannot exceed the upper limit"
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
                                 ? nullptr
                                 : &grade_stack_.grade_nodes[selected_grade_node_index_];
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
    } else if (point == QStringLiteral("end") && candidate.local_mask_kind == 1U) {
        candidate.local_mask_x1 = normalized_x;
        candidate.local_mask_y1 = normalized_y;
    } else if (point == QStringLiteral("center") && candidate.local_mask_kind == 2U) {
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
        setStatusMessage(local_mask_message(
            QT_TRANSLATE_NOOP("EditController", "A gradient mask needs two distinct points")
        ));
        return;
    }

    const BackendGradeStack before = grade_stack_;
    *grade_node = std::move(candidate);
    parameterEdited(QStringLiteral("local_mask/%1").arg(point), before);
    emit gradeNodesChanged();
}

void EditController::setSelectedLocalMaskInverted(const bool inverted) {
    auto* const grade_node = selected_grade_node_index_ < 0
                                 ? nullptr
                                 : &grade_stack_.grade_nodes[selected_grade_node_index_];
    if (!active_ || interactionLocked() || grade_node == nullptr || !grade_node->enabled
        || grade_node->local_mask_kind == 0U || grade_node->local_mask_invert == inverted) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    grade_node->local_mask_invert = inverted;
    parameterEdited(QStringLiteral("local_mask/invert"), before);
    emit gradeNodesChanged();
}

void EditController::appendSelectedLocalMaskBrushStroke(const QVariantList& points) {
    constexpr qsizetype maximum_brush_points = 4'096;
    constexpr double minimum_point_distance = 0.0015;
    auto* const grade_node = selected_grade_node_index_ < 0
                                 ? nullptr
                                 : &grade_stack_.grade_nodes[selected_grade_node_index_];
    if (!active_ || interactionLocked() || grade_node == nullptr || !grade_node->enabled
        || grade_node->local_mask_kind != 3U) {
        return;
    }
    const qsizetype point_count = grade_node->local_mask_brush_points.size() / 3;
    const qsizetype remaining_points = maximum_brush_points - point_count;
    if (remaining_points <= 0) {
        setStatusMessage(local_mask_message(
            QT_TRANSLATE_NOOP("EditController", "This brush mask has reached its point limit")
        ));
        return;
    }
    const auto normalized_points =
        EditStrokeInput::decodeNormalizedPoints(points, maximum_brush_points);
    if (!normalized_points.has_value()) {
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    QPointF previous;
    bool has_previous = false;
    for (const QPointF& point : *normalized_points) {
        if (grade_node->local_mask_brush_points.size() / 3 >= maximum_brush_points) {
            break;
        }
        if (has_previous
            && std::hypot(point.x() - previous.x(), point.y() - previous.y())
                   < minimum_point_distance) {
            continue;
        }
        grade_node->local_mask_brush_points.push_back(point.x());
        grade_node->local_mask_brush_points.push_back(point.y());
        grade_node->local_mask_brush_points.push_back(has_previous ? 0.0 : 1.0);
        previous = point;
        has_previous = true;
    }
    if (!has_previous) {
        return;
    }
    parameterEdited(QStringLiteral("local_mask/brush"), before);
    emit gradeNodesChanged();
    if (normalized_points->size() > remaining_points) {
        setStatusMessage(local_mask_message(
            QT_TRANSLATE_NOOP("EditController", "This brush mask has reached its point limit")
        ));
    }
}

void EditController::clearSelectedLocalMaskBrush() {
    auto* const grade_node = selected_grade_node_index_ < 0
                                 ? nullptr
                                 : &grade_stack_.grade_nodes[selected_grade_node_index_];
    if (!active_ || interactionLocked() || grade_node == nullptr || !grade_node->enabled
        || grade_node->local_mask_kind != 3U || grade_node->local_mask_brush_points.isEmpty()) {
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    grade_node->local_mask_brush_points.clear();
    parameterEdited(QStringLiteral("local_mask/brush/clear"), before);
    emit gradeNodesChanged();
}

void EditController::resetSelectedLocalMask() {
    auto* const grade_node = selected_grade_node_index_ < 0
                                 ? nullptr
                                 : &grade_stack_.grade_nodes[selected_grade_node_index_];
    if (!active_ || interactionLocked() || grade_node == nullptr || !grade_node->enabled
        || grade_node->local_mask_kind == 0U) {
        return;
    }
    BackendGradeNode reset = *grade_node;
    initializeLocalMask(reset, grade_node->local_mask_kind);
    if (reset == *grade_node) {
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    *grade_node = std::move(reset);
    parameterEdited(QStringLiteral("local_mask/reset"), before);
    emit gradeNodesChanged();
}
