#pragma once

// Internal scalar-field filtering contracts shared by tone and detail execution.
// The prepared guided-filter aggregate owns its dimensions, radius, border policy,
// mean, and variance so callers cannot accidentally recombine incompatible fields.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace shadow::image::detail {

[[nodiscard]] std::size_t reflect101_index(std::int64_t index, std::size_t extent) noexcept;

[[nodiscard]] std::vector<double> gaussian_kernel(double sigma, std::uint32_t radius);

[[nodiscard]] std::vector<double> gaussian_blur_scalar(std::span<const double> source,
                                                       std::size_t width, std::size_t height,
                                                       double sigma_x, double sigma_y);

class PreparedGuidedFilter final {
  public:
    PreparedGuidedFilter(const PreparedGuidedFilter&) = delete;
    PreparedGuidedFilter(PreparedGuidedFilter&&) noexcept = default;
    PreparedGuidedFilter& operator=(const PreparedGuidedFilter&) = delete;
    PreparedGuidedFilter& operator=(PreparedGuidedFilter&&) noexcept = default;
    ~PreparedGuidedFilter() = default;

    [[nodiscard]] std::size_t width() const noexcept { return width_; }
    [[nodiscard]] std::size_t height() const noexcept { return height_; }
    [[nodiscard]] std::uint32_t radius() const noexcept { return radius_; }

  private:
    PreparedGuidedFilter(std::vector<float> mean, std::vector<float> variance, std::size_t width,
                         std::size_t height, std::uint32_t radius) noexcept;

    std::vector<float> mean_;
    std::vector<float> variance_;
    std::size_t width_ = 0U;
    std::size_t height_ = 0U;
    std::uint32_t radius_ = 0U;

    friend PreparedGuidedFilter prepare_replicated_guided_filter(std::span<const float> guide,
                                                                 std::size_t width,
                                                                 std::size_t height,
                                                                 std::uint32_t radius);
    friend std::vector<float> apply_guided_self_filter(std::span<const float> guide,
                                                       const PreparedGuidedFilter& prepared,
                                                       double epsilon);
    friend std::vector<float> apply_guided_target_filter(std::span<const float> guide,
                                                         const PreparedGuidedFilter& prepared,
                                                         std::span<const float> target,
                                                         double epsilon);
};

// Guided-filter box means deliberately use replicated borders. Detail-tile
// preparation supplies the corresponding apron, so this policy differs from
// the reflect-101 boundary used by Gaussian convolution and selective tone.
[[nodiscard]] PreparedGuidedFilter prepare_replicated_guided_filter(std::span<const float> guide,
                                                                    std::size_t width,
                                                                    std::size_t height,
                                                                    std::uint32_t radius);

[[nodiscard]] std::vector<float> apply_guided_self_filter(std::span<const float> guide,
                                                          const PreparedGuidedFilter& prepared,
                                                          double epsilon);

[[nodiscard]] std::vector<float> apply_guided_target_filter(std::span<const float> guide,
                                                            const PreparedGuidedFilter& prepared,
                                                            std::span<const float> target,
                                                            double epsilon);

} // namespace shadow::image::detail
