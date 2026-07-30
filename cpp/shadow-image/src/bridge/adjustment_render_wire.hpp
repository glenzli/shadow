#pragma once

#include "rust/cxx.h"

#include <shadow/image/adjustment_graph.hpp>
#include <shadow/image/adjustment_layers.hpp>
#include <shadow/image/photo_liquify.hpp>

#include <optional>
#include <vector>

namespace shadow::bridge {

struct FfiAdjustmentNode;
struct FfiPhotoLiquify;

namespace adjustment_render_wire {

// Decode the flat adjustment-node wire contract. Validation order and diagnostics are part of
// the private Rust/C++ bridge contract and deliberately remain centralized in this owner.
[[nodiscard]] std::vector<image::AdjustmentNode> adjustment_nodes(
    const rust::Vec<FfiAdjustmentNode>& nodes
);

// Decode compiler-authored local-mask layer boundaries when present. A stream without boundaries
// returns nullopt so the caller can preserve the existing flat-node execution path and timing.
[[nodiscard]] std::optional<std::vector<image::AdjustmentLayer>> adjustment_layers(
    const rust::Vec<FfiAdjustmentNode>& source
);

/// Decode the optional photo-private structural payload. Absence has one
/// canonical empty wire form; presence must describe complete bounded push
/// paths and is revalidated by the native algorithm owner.
[[nodiscard]] std::optional<image::PhotoLiquify> photo_liquify(
    const FfiPhotoLiquify& source
);

} // namespace adjustment_render_wire

} // namespace shadow::bridge
