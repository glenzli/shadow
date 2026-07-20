#pragma once

#include "desktop_backend.hpp"

#include <algorithm>

namespace EditStack {

inline constexpr int minimum_layer_count = 1;
inline constexpr int maximum_layer_count = 16;

[[nodiscard]] inline int layerIndex(
    const BackendEditSettings& settings,
    const QString& layer_id
) noexcept {
    if (layer_id.isEmpty()) {
        return -1;
    }
    for (qsizetype index = 0; index < settings.layers.size(); ++index) {
        if (settings.layers.at(index).layer_id == layer_id) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

[[nodiscard]] inline int resolvedSelection(
    const BackendEditSettings& settings,
    const QString& preferred_layer_id,
    const int fallback_index
) noexcept {
    const int preferred = layerIndex(settings, preferred_layer_id);
    if (preferred >= 0) {
        return preferred;
    }
    if (settings.layers.isEmpty()) {
        return -1;
    }
    const int last = static_cast<int>(settings.layers.size() - 1);
    return std::clamp(fallback_index, 0, last);
}

[[nodiscard]] inline bool canInsert(
    const BackendEditSettings& settings,
    const BackendBasicEditLayer& layer
) noexcept {
    return settings.layers.size() < maximum_layer_count
        && !layer.layer_id.isEmpty()
        && layerIndex(settings, layer.layer_id) < 0;
}

inline bool insertAfterSelection(
    BackendEditSettings& settings,
    const BackendBasicEditLayer& layer,
    int& selected_index
) {
    if (!canInsert(settings, layer)) {
        return false;
    }
    const int count = static_cast<int>(settings.layers.size());
    const int insertion_index = selected_index >= 0 && selected_index < count
        ? selected_index + 1
        : count;
    settings.layers.insert(insertion_index, layer);
    selected_index = insertion_index;
    return true;
}

inline bool deleteSelection(
    BackendEditSettings& settings,
    int& selected_index
) {
    const int count = static_cast<int>(settings.layers.size());
    if (count <= minimum_layer_count || selected_index < 0
        || selected_index >= count) {
        return false;
    }
    settings.layers.removeAt(selected_index);
    selected_index = std::min(
        selected_index,
        static_cast<int>(settings.layers.size() - 1)
    );
    return true;
}

inline bool moveSelection(
    BackendEditSettings& settings,
    int& selected_index,
    const int destination_index
) {
    const int count = static_cast<int>(settings.layers.size());
    if (selected_index < 0 || selected_index >= count || destination_index < 0
        || destination_index >= count || destination_index == selected_index) {
        return false;
    }
    settings.layers.move(selected_index, destination_index);
    selected_index = destination_index;
    return true;
}

} // namespace EditStack
