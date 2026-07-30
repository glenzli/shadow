#pragma once

#include <shadow/image/raw_foundation.hpp>

namespace shadow::bridge {
struct FfiRawFoundation;
}

namespace shadow::bridge::raw_foundation_wire {

// Converts the Rust-owned wire value into one native borrowed view. Provenance strings are copied
// into the view; the large float payload remains a synchronous span over Rust memory.
[[nodiscard]] image::RawFoundationCameraRgbView
raw_foundation_view(const FfiRawFoundation& foundation);

} // namespace shadow::bridge::raw_foundation_wire
