#include "edit_ai_mask_controller.hpp"
#include "edit_controller.hpp"
#include "edit_mask_component_mutation.hpp"
#include "edit_stroke_input.hpp"

#include <QLatin1StringView>
#include <QUuid>

#include <algorithm>
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
    double BackendMaskComponent::* field = nullptr;
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
        .field = &BackendMaskComponent::x0,
        .kind_mask = mask_kind_bit(1U) | mask_kind_bit(2U),
    },
    LocalMaskParameterDescriptor{
        .key = "y0",
        .field = &BackendMaskComponent::y0,
        .kind_mask = mask_kind_bit(1U) | mask_kind_bit(2U),
    },
    LocalMaskParameterDescriptor{
        .key = "x1",
        .field = &BackendMaskComponent::x1,
        .kind_mask = mask_kind_bit(1U),
    },
    LocalMaskParameterDescriptor{
        .key = "y1",
        .field = &BackendMaskComponent::y1,
        .kind_mask = mask_kind_bit(1U),
    },
    LocalMaskParameterDescriptor{
        .key = "radiusX",
        .field = &BackendMaskComponent::radius_x,
        .kind_mask = mask_kind_bit(2U) | mask_kind_bit(3U),
        .minimum = 0.005,
    },
    LocalMaskParameterDescriptor{
        .key = "radiusY",
        .field = &BackendMaskComponent::radius_y,
        .kind_mask = mask_kind_bit(2U),
        .minimum = 0.01,
    },
    LocalMaskParameterDescriptor{
        .key = "feather",
        .field = &BackendMaskComponent::feather,
        .kind_mask = mask_kind_bit(2U) | mask_kind_bit(3U) | mask_kind_bit(6U),
    },
    LocalMaskParameterDescriptor{
        .key = "x0",
        .field = &BackendMaskComponent::x0,
        .kind_mask = mask_kind_bit(6U),
        .minimum = -1.0,
    },
    LocalMaskParameterDescriptor{
        .key = "lower",
        .field = &BackendMaskComponent::x0,
        .kind_mask = mask_kind_bit(4U),
    },
    LocalMaskParameterDescriptor{
        .key = "upper",
        .field = &BackendMaskComponent::x1,
        .kind_mask = mask_kind_bit(4U),
    },
    LocalMaskParameterDescriptor{
        .key = "softness",
        .field = &BackendMaskComponent::feather,
        .kind_mask = mask_kind_bit(4U) | mask_kind_bit(5U),
    },
    LocalMaskParameterDescriptor{
        .key = "centerHue",
        .field = &BackendMaskComponent::x0,
        .kind_mask = mask_kind_bit(5U),
        .maximum = 359.0 / 360.0,
    },
    LocalMaskParameterDescriptor{
        .key = "width",
        .field = &BackendMaskComponent::x1,
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

BackendMaskComponent* EditController::selectedLocalMaskComponent() noexcept {
    auto* const grade_node = selected_grade_node_index_ < 0
                                 ? nullptr
                                 : &grade_stack_.grade_nodes[selected_grade_node_index_];
    if (grade_node == nullptr || selected_local_mask_component_index_ < 0
        || selected_local_mask_component_index_ >= grade_node->local_mask_components.size()) {
        return nullptr;
    }
    return &grade_node->local_mask_components[selected_local_mask_component_index_];
}

const BackendMaskComponent* EditController::selectedLocalMaskComponent() const noexcept {
    const auto* const grade_node = selectedGradeNode();
    if (grade_node == nullptr || selected_local_mask_component_index_ < 0
        || selected_local_mask_component_index_ >= grade_node->local_mask_components.size()) {
        return nullptr;
    }
    return &grade_node->local_mask_components.at(selected_local_mask_component_index_);
}

void EditController::clampSelectedLocalMaskComponent() {
    const auto* const grade_node = selectedGradeNode();
    if (grade_node == nullptr || grade_node->local_mask_components.isEmpty()) {
        selected_local_mask_component_index_ = 0;
        return;
    }
    selected_local_mask_component_index_ = std::clamp(
        selected_local_mask_component_index_,
        0,
        static_cast<int>(grade_node->local_mask_components.size() - 1)
    );
}

int EditController::selectedLocalMaskComponentIndex() const noexcept {
    return selected_local_mask_component_index_;
}

QVariantList EditController::localMaskComponents() const {
    QVariantList result;
    const auto* const grade_node = selectedGradeNode();
    if (grade_node == nullptr) {
        return result;
    }
    result.reserve(grade_node->local_mask_components.size());
    for (qsizetype index = 0; index < grade_node->local_mask_components.size(); ++index) {
        const auto& component = grade_node->local_mask_components.at(index);
        result.push_back(
            QVariantMap{
                {QStringLiteral("componentId"), component.component_id},
                {QStringLiteral("operation"), static_cast<int>(component.operation)},
                {QStringLiteral("enabled"), component.enabled},
                {QStringLiteral("kind"), static_cast<int>(component.kind)},
                {QStringLiteral("selected"), index == selected_local_mask_component_index_},
            }
        );
    }
    return result;
}

QVariantMap EditController::selectedLocalMask() const {
    const auto* const grade_node = selectedGradeNode();
    const auto* const component = selectedLocalMaskComponent();
    if (grade_node == nullptr || component == nullptr) {
        return {
            {QStringLiteral("kind"), 0},
            {QStringLiteral("componentCount"), 0},
            {QStringLiteral("finalInverted"), false},
        };
    }
    QVariantList brush_points;
    brush_points.reserve(component->brush_points.size() / 3);
    for (qsizetype offset = 0; offset + 2 < component->brush_points.size(); offset += 3) {
        brush_points.push_back(
            QVariantMap{
                {QStringLiteral("x"), component->brush_points.at(offset)},
                {QStringLiteral("y"), component->brush_points.at(offset + 1)},
                {QStringLiteral("beginsStroke"), component->brush_points.at(offset + 2) == 1.0},
            }
        );
    }
    return {
        {QStringLiteral("componentId"), component->component_id},
        {QStringLiteral("componentIndex"), selected_local_mask_component_index_},
        {QStringLiteral("componentCount"), grade_node->local_mask_components.size()},
        {QStringLiteral("operation"), static_cast<int>(component->operation)},
        {QStringLiteral("enabled"), component->enabled},
        {QStringLiteral("kind"), static_cast<int>(component->kind)},
        {QStringLiteral("x0"), component->x0},
        {QStringLiteral("y0"), component->y0},
        {QStringLiteral("x1"), component->x1},
        {QStringLiteral("y1"), component->y1},
        {QStringLiteral("radiusX"), component->radius_x},
        {QStringLiteral("radiusY"), component->radius_y},
        {QStringLiteral("feather"), component->feather},
        {QStringLiteral("lower"), component->x0},
        {QStringLiteral("upper"), component->x1},
        {QStringLiteral("centerHue"), component->x0},
        {QStringLiteral("width"), component->x1},
        {QStringLiteral("softness"), component->feather},
        {QStringLiteral("leafInverted"), component->leaf_invert},
        {QStringLiteral("finalInverted"), grade_node->local_mask_invert},
        {QStringLiteral("brushPoints"), brush_points},
        {QStringLiteral("semanticQuery"), component->semantic_query},
        {QStringLiteral("semanticMaximumRegions"),
         static_cast<int>(component->semantic_maximum_regions)},
        {QStringLiteral("semanticScoreThresholdPercent"),
         static_cast<int>(component->semantic_score_threshold_percent)},
    };
}

bool EditController::hasCopiedNodeMask() const noexcept {
    return node_mask_clipboard_.has_value();
}

void EditController::setSelectedLocalMask(const int kind) {
    if (kind == 0) {
        removeSelectedLocalMaskComponent();
        return;
    }
    auto* const grade_node = selected_grade_node_index_ < 0
                                 ? nullptr
                                 : &grade_stack_.grade_nodes[selected_grade_node_index_];
    auto* const component = selectedLocalMaskComponent();
    if (!active_ || interactionLocked() || grade_node == nullptr || component == nullptr
        || !grade_node->enabled || kind < 1 || kind > 5 || component->kind == kind) {
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    const QString component_id = component->component_id;
    const std::uint8_t operation = component->operation;
    const bool enabled = component->enabled;
    initializeLocalMaskComponent(*component, kind, operation);
    component->component_id = component_id;
    component->enabled = enabled;
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
        || grade_node->local_mask_components.isEmpty()) {
        setStatusMessage(
            local_mask_message(QT_TRANSLATE_NOOP("EditController", "Select a node mask to copy"))
        );
        return;
    }
    if (std::ranges::any_of(
            grade_node->local_mask_components,
            [](const BackendMaskComponent& component) { return component.kind == 6U; }
        )) {
        setStatusMessage(local_mask_message(QT_TRANSLATE_NOOP(
            "EditController",
            "Copy AI mask components by re-evaluating their semantic selection on the target photo"
        )));
        return;
    }
    finishActiveGesture();
    node_mask_clipboard_ = NodeMaskClipboard{
        .components = grade_node->local_mask_components,
        .final_invert = grade_node->local_mask_invert,
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
    candidate.local_mask_components = source.components;
    for (auto& component : candidate.local_mask_components) {
        component.component_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    candidate.local_mask_invert = source.final_invert;
    if (candidate == *grade_node) {
        setStatusMessage(local_mask_message(
            QT_TRANSLATE_NOOP("EditController", "Selected Grade Node already uses the copied mask")
        ));
        return;
    }
    *grade_node = std::move(candidate);
    selected_local_mask_component_index_ = 0;
    parameterEdited(QStringLiteral("local_mask/paste"), before);
    emit gradeNodesChanged();
    setStatusMessage(local_mask_message(QT_TRANSLATE_NOOP("EditController", "Pasted node mask")));
}

void EditController::setSelectedLocalMaskValue(const QString& key, const double value) {
    auto* const component = selectedLocalMaskComponent();
    if (component == nullptr) {
        return;
    }
    const auto* const descriptor = local_mask_parameter(key, component->kind);
    if (descriptor == nullptr
        || !acceptParameter(
            value,
            descriptor->minimum,
            descriptor->maximum,
            QT_TRANSLATE_NOOP("EditController", "Local mask")
        )
        || component->*(descriptor->field) == value) {
        return;
    }

    BackendMaskComponent candidate = *component;
    candidate.*(descriptor->field) = value;
    if (candidate.kind == 1U
        && std::hypot(candidate.x1 - candidate.x0, candidate.y1 - candidate.y0) < 0.01) {
        setStatusMessage(local_mask_message(
            QT_TRANSLATE_NOOP("EditController", "A gradient mask needs two distinct points")
        ));
        return;
    }
    if (candidate.kind == 2U && (candidate.radius_x < 0.01 || candidate.radius_y < 0.01)) {
        setStatusMessage(local_mask_message(
            QT_TRANSLATE_NOOP("EditController", "A radial mask needs a non-zero radius")
        ));
        return;
    }
    if (candidate.kind == 3U && candidate.radius_x < 0.005) {
        setStatusMessage(local_mask_message(
            QT_TRANSLATE_NOOP("EditController", "A brush mask needs a non-zero size")
        ));
        return;
    }
    if (candidate.kind == 4U && candidate.x0 > candidate.x1) {
        setStatusMessage(local_mask_message(QT_TRANSLATE_NOOP(
            "EditController",
            "The lower lightness limit cannot exceed the upper limit"
        )));
        return;
    }

    const BackendGradeStack before = grade_stack_;
    *component = std::move(candidate);
    parameterEdited(QStringLiteral("local_mask/%1").arg(key), before);
    emit gradeNodesChanged();
}

void EditController::setSelectedLocalMaskPoint(
    const QString& point,
    const double normalized_x,
    const double normalized_y
) {
    auto* const component = selectedLocalMaskComponent();
    if (component == nullptr
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

    BackendMaskComponent candidate = *component;
    if (point == QStringLiteral("start") && candidate.kind == 1U) {
        candidate.x0 = normalized_x;
        candidate.y0 = normalized_y;
    } else if (point == QStringLiteral("end") && candidate.kind == 1U) {
        candidate.x1 = normalized_x;
        candidate.y1 = normalized_y;
    } else if (point == QStringLiteral("center") && candidate.kind == 2U) {
        candidate.x0 = normalized_x;
        candidate.y0 = normalized_y;
    } else {
        return;
    }

    if (candidate == *component) {
        return;
    }
    if (candidate.kind == 1U
        && std::hypot(candidate.x1 - candidate.x0, candidate.y1 - candidate.y0) < 0.01) {
        setStatusMessage(local_mask_message(
            QT_TRANSLATE_NOOP("EditController", "A gradient mask needs two distinct points")
        ));
        return;
    }

    const BackendGradeStack before = grade_stack_;
    *component = std::move(candidate);
    parameterEdited(QStringLiteral("local_mask/%1").arg(point), before);
    emit gradeNodesChanged();
}

void EditController::setSelectedLocalMaskInverted(const bool inverted) {
    auto* const grade_node = selected_grade_node_index_ < 0
                                 ? nullptr
                                 : &grade_stack_.grade_nodes[selected_grade_node_index_];
    if (!active_ || interactionLocked() || grade_node == nullptr || !grade_node->enabled
        || grade_node->local_mask_components.isEmpty()
        || grade_node->local_mask_invert == inverted) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    grade_node->local_mask_invert = inverted;
    parameterEdited(QStringLiteral("local_mask/invert"), before);
    emit gradeNodesChanged();
}

void EditController::setSelectedLocalMaskLeafInverted(const bool inverted) {
    auto* const grade_node = selected_grade_node_index_ < 0
                                 ? nullptr
                                 : &grade_stack_.grade_nodes[selected_grade_node_index_];
    auto* const component = selectedLocalMaskComponent();
    if (!active_ || interactionLocked() || grade_node == nullptr || component == nullptr
        || !grade_node->enabled || component->leaf_invert == inverted) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    component->leaf_invert = inverted;
    parameterEdited(QStringLiteral("local_mask/component/invert"), before);
    emit gradeNodesChanged();
}

void EditController::appendSelectedLocalMaskBrushStroke(const QVariantList& points) {
    constexpr qsizetype maximum_brush_points = 4'096;
    constexpr double minimum_point_distance = 0.0015;
    auto* const grade_node = selected_grade_node_index_ < 0
                                 ? nullptr
                                 : &grade_stack_.grade_nodes[selected_grade_node_index_];
    auto* const component = selectedLocalMaskComponent();
    if (!active_ || interactionLocked() || grade_node == nullptr || component == nullptr
        || !grade_node->enabled || component->kind != 3U) {
        return;
    }
    const qsizetype point_count = component->brush_points.size() / 3;
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
        if (component->brush_points.size() / 3 >= maximum_brush_points) {
            break;
        }
        if (has_previous
            && std::hypot(point.x() - previous.x(), point.y() - previous.y())
                   < minimum_point_distance) {
            continue;
        }
        component->brush_points.push_back(point.x());
        component->brush_points.push_back(point.y());
        component->brush_points.push_back(has_previous ? 0.0 : 1.0);
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
    auto* const component = selectedLocalMaskComponent();
    if (!active_ || interactionLocked() || grade_node == nullptr || component == nullptr
        || !grade_node->enabled || component->kind != 3U || component->brush_points.isEmpty()) {
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    component->brush_points.clear();
    parameterEdited(QStringLiteral("local_mask/brush/clear"), before);
    emit gradeNodesChanged();
}

void EditController::resetSelectedLocalMask() {
    auto* const grade_node = selected_grade_node_index_ < 0
                                 ? nullptr
                                 : &grade_stack_.grade_nodes[selected_grade_node_index_];
    auto* const component = selectedLocalMaskComponent();
    if (!active_ || interactionLocked() || grade_node == nullptr || component == nullptr
        || !grade_node->enabled) {
        return;
    }
    BackendMaskComponent reset;
    initializeLocalMaskComponent(reset, component->kind, component->operation);
    reset.component_id = component->component_id;
    reset.enabled = component->enabled;
    if (reset == *component) {
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    *component = std::move(reset);
    parameterEdited(QStringLiteral("local_mask/reset"), before);
    emit gradeNodesChanged();
}

void EditController::selectLocalMaskComponent(const int index) {
    const auto* const grade_node = selectedGradeNode();
    if (grade_node == nullptr || index < 0 || index >= grade_node->local_mask_components.size()
        || selected_local_mask_component_index_ == index) {
        return;
    }
    finishActiveGesture();
    selected_local_mask_component_index_ = index;
    notifyParametersChanged();
}

void EditController::setSelectedLocalMaskComponentEnabled(const bool enabled) {
    auto* const grade_node = selected_grade_node_index_ < 0
                                 ? nullptr
                                 : &grade_stack_.grade_nodes[selected_grade_node_index_];
    auto* const component = selectedLocalMaskComponent();
    if (!active_ || interactionLocked() || grade_node == nullptr || component == nullptr
        || !grade_node->enabled || component->enabled == enabled) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    component->enabled = enabled;
    parameterEdited(QStringLiteral("local_mask/component/enabled"), before);
    emit gradeNodesChanged();
}

void EditController::setSelectedLocalMaskComponentOperation(const int operation) {
    auto* const grade_node = selected_grade_node_index_ < 0
                                 ? nullptr
                                 : &grade_stack_.grade_nodes[selected_grade_node_index_];
    auto* const component = selectedLocalMaskComponent();
    if (!active_ || interactionLocked() || grade_node == nullptr || component == nullptr
        || !grade_node->enabled || selected_local_mask_component_index_ == 0 || operation < 1
        || operation > 3 || component->operation == operation) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    component->operation = static_cast<std::uint8_t>(operation);
    parameterEdited(QStringLiteral("local_mask/component/operation"), before);
    emit gradeNodesChanged();
}

void EditController::removeSelectedLocalMaskComponent() {
    auto* const grade_node = selected_grade_node_index_ < 0
                                 ? nullptr
                                 : &grade_stack_.grade_nodes[selected_grade_node_index_];
    if (!active_ || interactionLocked() || grade_node == nullptr || !grade_node->enabled
        || grade_node->local_mask_components.isEmpty() || selected_local_mask_component_index_ < 0
        || selected_local_mask_component_index_ >= grade_node->local_mask_components.size()) {
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    const auto removal = EditMaskComponentMutation::removeSelected(
        *grade_node,
        selected_local_mask_component_index_
    );
    if (removal == EditMaskComponentRemovalResult::Invalid) {
        return;
    }
    parameterEdited(QStringLiteral("local_mask/component/remove"), before);
    emit gradeNodesChanged();
    setStatusMessage(local_mask_message(
        removal == EditMaskComponentRemovalResult::RemovedNodeMask
            ? QT_TRANSLATE_NOOP("EditController", "Removed node mask")
            : QT_TRANSLATE_NOOP("EditController", "Removed mask component")
    ));
}
