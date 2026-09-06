#pragma once
#include <filesystem>
#include <optional>
#include <shadow/image/decoder_session.hpp>
#include <shadow/image/raw_development.hpp>
#include <string>
namespace shadow::image {
// Optional parallel interface: does not change the private DecodeSession provider ABI.
class LinearRasterSource {
  public:
    virtual ~LinearRasterSource() = default;
    [[nodiscard]] virtual std::optional<SceneLinearRgbFrame>
    linear_raster(std::optional<std::uint32_t> max_edge) const = 0;
};
[[nodiscard]] bool is_shadow_linear_tiff(const std::filesystem::path& path);
[[nodiscard]] std::unique_ptr<DecodeSession> open_linear_tiff(const std::filesystem::path& path);
// Only Shadow's explicit linear-sRGB fp32 TIFF contract is admitted. No guessed ICC/gamma.
[[nodiscard]] SceneLinearRgbFrame read_linear_tiff(
    const std::filesystem::path& path,
    std::optional<std::uint32_t> max_edge = std::nullopt
);
void write_linear_tiff(
    const std::filesystem::path& path,
    const SceneLinearRgbFrame& frame,
    const std::string& provenance_json
);
} // namespace shadow::image
