#pragma once

// Legacy MacTypes declares a global `shadow` enumerator. Rename only that SDK token while the
// Apple headers are parsed so it cannot collide with Shadow's top-level C++ namespace.
#define shadow shadow_mactypes_legacy_symbol
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#undef shadow

#include <cstddef>
#include <mutex>
#include <string>

namespace shadow::image::detail {

class OwnedObjectiveCObject final {
public:
    explicit OwnedObjectiveCObject(id value = nil) noexcept
        : value_(value) {}

    ~OwnedObjectiveCObject() {
        [value_ release];
    }

    OwnedObjectiveCObject(const OwnedObjectiveCObject&) = delete;
    OwnedObjectiveCObject& operator=(const OwnedObjectiveCObject&) = delete;

    [[nodiscard]] id get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept { return value_ != nil; }

private:
    id value_;
};

[[nodiscard]] id<MTLDevice> metal_raw_device() noexcept;
[[nodiscard]] id<MTLCommandQueue> metal_raw_command_queue() noexcept;
[[nodiscard]] id<MTLComputePipelineState> metal_raw_reconstruction_pipeline() noexcept;
[[nodiscard]] id<MTLComputePipelineState> metal_raw_area_preview_pipeline() noexcept;
[[nodiscard]] id<MTLComputePipelineState> metal_raw_denoise_pipeline() noexcept;
[[nodiscard]] id<MTLComputePipelineState> metal_dcp_color_pipeline() noexcept;

[[nodiscard]] bool metal_raw_area_preview_available() noexcept;
[[nodiscard]] const std::string& metal_raw_runtime_diagnostic() noexcept;
[[nodiscard]] const std::string& metal_raw_area_preview_diagnostic() noexcept;
[[nodiscard]] const std::string& metal_raw_denoise_diagnostic() noexcept;
[[nodiscard]] const std::string& metal_dcp_color_diagnostic() noexcept;

[[nodiscard]] std::mutex& metal_execution_mutex();
[[nodiscard]] bool checked_multiply(
    std::size_t left,
    std::size_t right,
    std::size_t& result
) noexcept;
[[nodiscard]] bool checked_add(
    std::size_t left,
    std::size_t right,
    std::size_t& result
) noexcept;
[[nodiscard]] std::string metal_raw_command_buffer_diagnostic(
    id<MTLCommandBuffer> command_buffer
);

} // namespace shadow::image::detail
