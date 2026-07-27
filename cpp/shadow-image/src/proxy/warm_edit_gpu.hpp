#pragma once

#include <shadow/image/decoder_types.hpp>
#include <shadow/image/edit.hpp>

#include "../edit/adjustment_execution_internal.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace shadow::image::detail {

struct WarmEditGpuPreparation;

// The warm backend owns one immutable source upload and two independently synchronized output
// slots. It is deliberately scoped to one WarmEditPreviewSession: process-global state is
// limited to immutable Metal device/pipeline objects, so unrelated photos never serialize on a
// global execution lock.
class WarmEditGpuSession final {
public:
    WarmEditGpuSession(const WarmEditGpuSession&) = delete;
    WarmEditGpuSession& operator=(const WarmEditGpuSession&) = delete;
    ~WarmEditGpuSession();

    struct RenderResult final {
        Dimensions dimensions;
        std::vector<std::uint8_t> rgb8;
        // Settled previews retain the pre-display scene-linear result for the exact existing
        // host analysis contract. Interactive previews leave this empty and avoid a large
        // device-to-host float copy.
        std::optional<FloatRgbImage> analyzed_linear;
        bool had_active_adjustments = false;
    };

    enum class RenderStatus : std::uint8_t {
        completed,
        cancelled,
        unavailable_or_failed,
    };

    struct RenderAttempt final {
        RenderStatus status = RenderStatus::unavailable_or_failed;
        std::optional<RenderResult> output;
        std::string diagnostic;
    };

    [[nodiscard]] RenderAttempt render(
        std::span<const AdjustmentNode> nodes,
        const EditExecutionPlan& plan,
        bool retain_linear_for_analysis,
        std::stop_token cancellation = {}
    ) const;

    [[nodiscard]] WarmEditPreviewGpuStats stats() const noexcept;

private:
    struct Impl;
    explicit WarmEditGpuSession(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;

    friend struct WarmEditGpuPreparation;
    friend WarmEditGpuPreparation prepare_warm_edit_gpu_session(
        const FloatRgbImage& source
    );
};

struct WarmEditGpuPreparation final {
    std::shared_ptr<WarmEditGpuSession> session;
    std::string diagnostic;
};

[[nodiscard]] WarmEditGpuPreparation prepare_warm_edit_gpu_session(
    const FloatRgbImage& source
);

} // namespace shadow::image::detail
