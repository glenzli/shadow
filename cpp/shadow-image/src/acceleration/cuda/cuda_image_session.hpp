#pragma once

#include "../../edit/metal_adjustment_program.hpp"
#include <memory>
#include <shadow/image/warm_edit_preview.hpp>
#include <span>
#include <stop_token>

namespace shadow::image::detail {

struct CudaAdjustmentStage final {
    PreparedMetalAdjustment program;
    float opacity = 1.0F;
};

struct CudaRenderOutput final {
    std::vector<std::uint8_t> rgb8;
    std::optional<FloatRgbImage> linear;
};

// The CUDA implementation is linked only by CUDA-enabled native builds. All failures leave
// publication to the caller; no partially completed device result is returned.
class CudaImageSession final {
  public:
    explicit CudaImageSession(const FloatRgbImage& source);
    ~CudaImageSession();
    CudaImageSession(const CudaImageSession&) = delete;
    CudaImageSession& operator=(const CudaImageSession&) = delete;
    [[nodiscard]] const FloatRgbImage& layout() const noexcept;
    [[nodiscard]] std::optional<CudaRenderOutput> render(
        std::span<const CudaAdjustmentStage> stages,
        bool retain_linear,
        std::uint32_t origin_x,
        std::uint32_t origin_y,
        std::stop_token cancellation
    ) const;
    [[nodiscard]] std::shared_ptr<const FloatRgbImage> host_source() const;
    [[nodiscard]] WarmEditPreviewGpuStats stats() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace shadow::image::detail
